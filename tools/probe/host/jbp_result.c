/*
 * Probe results in memory and in .jbpr files (jbp_result.h, PROTOCOL.md).
 */
#include "jbp_result.h"

#include <stdlib.h>
#include <string.h>

static const char file_magic[8] = {'J', 'B', 'P', 'R', 'O', 'B', 'E', 0};
#define FILE_VERSION 4u
/* No result file is near this (VRAM is 1 MB, everything else far less). */
#define FILE_MAX ((size_t)8 << 20)

int jbp_result_init(jbp_result *r)
{
    memset(r, 0, sizeof *r);
    r->vram = calloc((size_t)JBP_VRAM_W * JBP_VRAM_H, sizeof *r->vram);
    return r->vram != NULL ? 0 : -1;
}

void jbp_result_free(jbp_result *r)
{
    free(r->reads);
    free(r->expects);
    free(r->waits);
    free(r->vram);
    memset(r, 0, sizeof *r);
}

/* The next part of a list of items (each `words` words): its first index
   must be the count so far, it must hold at least one item, and the count
   may not pass cap. */
static int put_items(uint32_t **arr, uint32_t *count, uint32_t words, uint32_t cap, const uint8_t *p,
                     size_t n)
{
    uint32_t first, m, i;
    uint32_t *more;

    if (n < 4 + 4 * (size_t)words || (n - 4) % (4 * (size_t)words) != 0 ||
        (n - 4) / (4 * (size_t)words) > cap)
        return -1;
    first = jbp_get32(p);
    m = (uint32_t)((n - 4) / (4 * (size_t)words));
    if (first != *count || m > cap - *count)
        return -1;
    more = realloc(*arr, ((size_t)*count + m) * words * sizeof *more);
    if (more == NULL)
        return -1;
    *arr = more;
    for (i = 0; i < m * words; ++i)
        more[(size_t)*count * words + i] = jbp_get32(p + 4 + 4 * (size_t)i);
    *count += m;
    return 0;
}

int jbp_result_apply(jbp_result *r, unsigned type, const uint8_t *p, size_t n)
{
    size_t i;

    switch (type) {
    case JBP_HELLO:
        /* Any HELLO heard; the session's R_IDENT takes precedence. */
        if (n != JBP_HELLO_BYTES)
            return -1;
        if (!r->have_ident)
            memcpy(r->hello, p, n);
        r->have_hello = 1;
        return 0;
    case JBP_R_IDENT:
        if (n != JBP_HELLO_BYTES || r->have_ident)
            return -1;
        memcpy(r->hello, p, n);
        r->have_hello = r->have_ident = 1;
        return 0;
    case JBP_R_HEAD:
        if (n != 4 * (size_t)JBP_H_WORDS || r->have_head)
            return -1;
        for (i = 0; i < JBP_H_WORDS; ++i)
            r->head[i] = jbp_get32(p + 4 * i);
        r->have_head = 1;
        return 0;
    case JBP_R_READS:
        return put_items(&r->reads, &r->nreads, 1, JBP_READS_MAX, p, n);
    case JBP_R_EXPECTS:
        return put_items(&r->expects, &r->nexpects, JBP_EXPECT_WORDS, JBP_EXPECTS_MAX, p, n);
    case JBP_R_WAITS:
        return put_items(&r->waits, &r->nwaits, JBP_WAITFAIL_WORDS, JBP_WAITFAIL_KEPT, p, n);
    case JBP_R_VRAM: {
        uint32_t y, rows, k;
        if (n != 8 + (size_t)JBP_VRAM_ROWS * 2048)
            return -1;
        y = jbp_get32(p);
        rows = jbp_get32(p + 4);
        if (rows != JBP_VRAM_ROWS || y != r->next_row || y >= JBP_VRAM_H)
            return -1;
        for (k = 0; k < rows * JBP_VRAM_W; ++k)
            r->vram[(size_t)y * JBP_VRAM_W + k] = (uint16_t)(p[8 + 2 * k] | p[9 + 2 * k] << 8);
        r->next_row += rows;
        return 0;
    }
    case JBP_R_DONE:
        if (n != JBP_DONE_BYTES || r->have_done)
            return -1;
        r->done_crc = jbp_get32(p);
        r->done_frames = jbp_get32(p + 4);
        r->done_resent = jbp_get32(p + 8);
        r->done_rx_bad = jbp_get32(p + 12);
        r->done_sio_errors = jbp_get32(p + 16);
        r->have_done = 1;
        return 0;
    case JBP_R_ERROR:
        if (n != JBP_ERROR_BYTES || r->have_error)
            return -1;
        for (i = 0; i < 3; ++i)
            r->error[i] = jbp_get32(p + 4 * i);
        r->have_error = 1;
        return 0;
    case JBP_REC_HOST: {
        /* name\0 bytes(4) crc(4) note\0, each string with exactly one
           zero byte, its last */
        size_t a = 0, m;
        if (r->have_host)
            return -1;
        while (a < n && p[a])
            ++a;
        if (a >= sizeof r->list_name || n - a < 10)
            return -1;
        m = n - (a + 9) - 1; /* the note's length, without its zero */
        if (m >= sizeof r->note || p[n - 1] != 0)
            return -1;
        for (i = 0; i < m; ++i)
            if (p[a + 9 + i] == 0)
                return -1;
        memcpy(r->list_name, p, a);
        r->list_name[a] = 0;
        r->list_bytes = jbp_get32(p + a + 1);
        r->list_crc = jbp_get32(p + a + 5);
        memcpy(r->note, p + a + 9, m);
        r->note[m] = 0;
        r->have_host = 1;
        return 0;
    }
    default:
        return -1; /* not a result's */
    }
}

uint32_t jbp_result_vram_crc(const jbp_result *r)
{
    uint8_t row[2 * JBP_VRAM_W];
    uint32_t crc = 0;
    int y, x;

    for (y = 0; y < JBP_VRAM_H; ++y) {
        for (x = 0; x < JBP_VRAM_W; ++x) {
            uint16_t v = r->vram[(size_t)y * JBP_VRAM_W + x];
            row[2 * x] = (uint8_t)v;
            row[2 * x + 1] = (uint8_t)(v >> 8);
        }
        crc = jbp_crc32(crc, row, sizeof row);
    }
    return crc;
}

const char *jbp_error_name(uint32_t code)
{
    static const char *const names[] = {
        "none", "not a list", "a list version the probe does not know", "a bad op",
        "the main loop is larger than the probe can hold", "the list's length is not START's",
        "the list's CRC-32 is not START's", "the host went quiet", "another protocol version",
        "a main loop without a pass, or a pass without one", "a frame the protocol does not allow",
        "the GPU was not ready for a command at a read-back", "the GPU never offered VRAM to read",
        "more reads than the probe keeps", "more checks than the probe keeps",
        "the GPU was not ready for a command before the list"};

    return code < sizeof names / sizeof names[0] ? names[code] : "unknown";
}

int jbp_result_check(const jbp_result *r, char *why, size_t size)
{
    const uint32_t *d = r->head;
    uint32_t k, failed = 0, kept;

    if (r->have_error) {
        snprintf(why, size, "the probe reported error %u (%s) at word %u (%u bytes received)",
                 (unsigned)r->error[0], jbp_error_name(r->error[0]), (unsigned)r->error[1],
                 (unsigned)r->error[2]);
        return -1;
    }
    if (!r->have_ident || !r->have_head || !r->have_done || !r->have_host) {
        snprintf(why, size, "incomplete:%s%s%s%s missing", r->have_ident ? "" : " identification",
                 r->have_head ? "" : " summary", r->have_done ? "" : " end",
                 r->have_host ? "" : " host record");
        return -1;
    }
    if (jbp_get32(r->hello + 4 * JBP_ID_PROTOCOL) != JBP_PROTOCOL_VERSION) {
        snprintf(why, size, "from a probe of protocol %u, not %u",
                 (unsigned)jbp_get32(r->hello + 4 * JBP_ID_PROTOCOL), (unsigned)JBP_PROTOCOL_VERSION);
        return -1;
    }
    if (d[JBP_H_STATUS] != 0) {
        snprintf(why, size, "status %u", (unsigned)d[JBP_H_STATUS]);
        return -1;
    }
    if (d[JBP_H_LIST_BYTES] != r->list_bytes || d[JBP_H_LIST_CRC] != r->list_crc) {
        snprintf(why, size, "the probe played a list of %u bytes, CRC-32 0x%08x; the list sent was "
                 "%u bytes, CRC-32 0x%08x",
                 (unsigned)d[JBP_H_LIST_BYTES], (unsigned)d[JBP_H_LIST_CRC], (unsigned)r->list_bytes,
                 (unsigned)r->list_crc);
        return -1;
    }
    if (r->next_row != JBP_VRAM_H || r->done_frames != JBP_VRAM_H / JBP_VRAM_ROWS) {
        snprintf(why, size, "VRAM incomplete: %u rows received, %u frames by the probe's count",
                 (unsigned)r->next_row, (unsigned)r->done_frames);
        return -1;
    }
    if (jbp_result_vram_crc(r) != r->done_crc) {
        snprintf(why, size, "VRAM CRC-32 0x%08x, the probe's 0x%08x", (unsigned)jbp_result_vram_crc(r),
                 (unsigned)r->done_crc);
        return -1;
    }
    if (d[JBP_H_LOOP]) {
        /* The main loop's rule (FORMAT.md, "Playing a list"). */
        if (!d[JBP_H_STEADY]) {
            snprintf(why, size, "the main loop is not at a fixed point at pass %u: pass %u (of the "
                     "window of %u after it) changed VRAM", (unsigned)d[JBP_H_PASSES],
                     (unsigned)d[JBP_H_CHANGED_AT], (unsigned)d[JBP_H_WINDOW]);
            return -1;
        }
        if (d[JBP_H_EARLY]) {
            snprintf(why, size, "the main loop had settled before pass %u: pass %u left VRAM as "
                     "pass %u had (the pass count is not the first of the fixed point)",
                     (unsigned)d[JBP_H_PASSES], (unsigned)d[JBP_H_PASSES],
                     (unsigned)d[JBP_H_PASSES] - 1);
            return -1;
        }
        if (d[JBP_H_PASSES] == 0 || d[JBP_H_PASSES] > JBP_PASSES_MAX || d[JBP_H_WINDOW] == 0 ||
            d[JBP_H_WINDOW] > JBP_PASSES_MAX || d[JBP_H_PLAYED] != d[JBP_H_PASSES] + d[JBP_H_WINDOW] ||
            d[JBP_H_CRC_AT] != r->done_crc || d[JBP_H_CRC_LAST] != r->done_crc) {
            snprintf(why, size, "the main loop's summary is inconsistent: pass %u, window %u, "
                     "%u played; CRC-32 after pass %u 0x%08x, at the end 0x%08x, VRAM sent 0x%08x",
                     (unsigned)d[JBP_H_PASSES], (unsigned)d[JBP_H_WINDOW], (unsigned)d[JBP_H_PLAYED],
                     (unsigned)d[JBP_H_PASSES], (unsigned)d[JBP_H_CRC_AT], (unsigned)d[JBP_H_CRC_LAST],
                     (unsigned)r->done_crc);
            return -1;
        }
    } else if (d[JBP_H_PASSES] != 0) {
        snprintf(why, size, "no main loop, but pass %u stated", (unsigned)d[JBP_H_PASSES]);
        return -1;
    }
    /* Every read, check and kept wait the summary counts, and no more. */
    if (r->nreads != d[JBP_H_READS] || d[JBP_H_READS_KEPT] != d[JBP_H_READS]) {
        snprintf(why, size, "%u reads received; the probe read %u and kept %u", (unsigned)r->nreads,
                 (unsigned)d[JBP_H_READS], (unsigned)d[JBP_H_READS_KEPT]);
        return -1;
    }
    if (r->nexpects != d[JBP_H_EXPECTS]) {
        snprintf(why, size, "%u checks received; the probe evaluated %u", (unsigned)r->nexpects,
                 (unsigned)d[JBP_H_EXPECTS]);
        return -1;
    }
    for (k = 0; k < r->nexpects; ++k) {
        const uint32_t *e = r->expects + (size_t)k * JBP_EXPECT_WORDS;
        if (e[1] >= r->nreads || e[4] != r->reads[e[1]]) {
            snprintf(why, size, "check %u names read %u, and does not hold what was read there",
                     (unsigned)k, (unsigned)e[1]);
            return -1;
        }
        failed += (e[4] & e[2]) != e[3];
    }
    if (failed != d[JBP_H_EXPECTS_FAILED]) {
        snprintf(why, size, "%u checks fail as received; the probe counted %u", (unsigned)failed,
                 (unsigned)d[JBP_H_EXPECTS_FAILED]);
        return -1;
    }
    kept = d[JBP_H_WAITS_FAILED] < JBP_WAITFAIL_KEPT ? d[JBP_H_WAITS_FAILED] : JBP_WAITFAIL_KEPT;
    if (r->nwaits != kept || d[JBP_H_WAITS_FAILED] > d[JBP_H_WAITS]) {
        snprintf(why, size, "%u failed waits received; the probe counted %u of %u (the first %u are "
                 "sent)", (unsigned)r->nwaits, (unsigned)d[JBP_H_WAITS_FAILED], (unsigned)d[JBP_H_WAITS],
                 (unsigned)JBP_WAITFAIL_KEPT);
        return -1;
    }
    return 0;
}

int jbp_result_clean(const jbp_result *r, char *why, size_t size)
{
    const uint32_t *d = r->head;

    if (jbp_result_check(r, why, size) != 0)
        return -1;
    if (d[JBP_H_WAITS_FAILED] != 0) {
        /* jbp_result_check has made sure the first of them was kept. */
        snprintf(why, size, "not clean: %u of %u waits (WAIT, DRAWSYNC) timed out, the first at word "
                 "%u in pass %u (GPUSTAT 0x%08x, & 0x%08x is not 0x%08x)",
                 (unsigned)d[JBP_H_WAITS_FAILED], (unsigned)d[JBP_H_WAITS], (unsigned)r->waits[0],
                 (unsigned)r->waits[4], (unsigned)r->waits[3], (unsigned)r->waits[1],
                 (unsigned)r->waits[2]);
        return -1;
    }
    if (d[JBP_H_DMA_TIMEOUTS] != 0) {
        snprintf(why, size, "not clean: %u of %u DMA transfers timed out", (unsigned)d[JBP_H_DMA_TIMEOUTS],
                 (unsigned)d[JBP_H_DMA_OPS]);
        return -1;
    }
    if (d[JBP_H_VSYNC_TIMEOUTS] != 0) {
        snprintf(why, size, "not clean: %u of %u vertical blanks timed out",
                 (unsigned)d[JBP_H_VSYNC_TIMEOUTS], (unsigned)d[JBP_H_VSYNCS]);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Files: magic, version, then records (type, bytes, payload padded to a
   word), all little-endian; the payloads are the frames' (PROTOCOL.md). */

static int put_word(FILE *f, uint32_t v)
{
    uint8_t b[4];

    jbp_put32(b, v);
    return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}

static int put_record(FILE *f, unsigned type, const uint8_t *p, size_t n)
{
    static const uint8_t zero[4] = {0, 0, 0, 0};

    if (put_word(f, type) || put_word(f, (uint32_t)n) || (n && fwrite(p, 1, n, f) != n))
        return -1;
    if (n % 4 && fwrite(zero, 1, 4 - n % 4, f) != 4 - n % 4)
        return -1;
    return 0;
}

/* All the items in one record, from index 0; none: no record. */
static int put_items_rec(FILE *f, unsigned type, const uint32_t *w, uint32_t count, uint32_t words)
{
    uint8_t *b;
    uint32_t i;
    int e;

    if (count == 0)
        return 0;
    b = malloc(4 + (size_t)count * words * 4);
    if (b == NULL)
        return -1;
    jbp_put32(b, 0);
    for (i = 0; i < count * words; ++i)
        jbp_put32(b + 4 + 4 * (size_t)i, w[i]);
    e = put_record(f, type, b, 4 + (size_t)count * words * 4);
    free(b);
    return e;
}

int jbp_result_save(const jbp_result *r, const char *path, char *err, size_t size)
{
    FILE *f = fopen(path, "wb");
    uint8_t b[8 + 2 * 2048];
    int e = 0, y;
    size_t i;

    if (f == NULL) {
        snprintf(err, size, "cannot write %s", path);
        return -1;
    }
    e |= fwrite(file_magic, 1, 8, f) != 8;
    e |= put_word(f, FILE_VERSION);
    {
        size_t a = strlen(r->list_name), m = strlen(r->note);
        uint8_t *h = malloc(a + m + 10);
        if (h == NULL)
            e = 1;
        else {
            memcpy(h, r->list_name, a + 1);
            jbp_put32(h + a + 1, r->list_bytes);
            jbp_put32(h + a + 5, r->list_crc);
            memcpy(h + a + 9, r->note, m + 1);
            e |= put_record(f, JBP_REC_HOST, h, a + m + 10);
            free(h);
        }
    }
    if (r->have_hello)
        e |= put_record(f, r->have_ident ? JBP_R_IDENT : JBP_HELLO, r->hello, sizeof r->hello);
    if (r->have_head) {
        uint8_t h[4 * JBP_H_WORDS];
        for (i = 0; i < JBP_H_WORDS; ++i)
            jbp_put32(h + 4 * i, r->head[i]);
        e |= put_record(f, JBP_R_HEAD, h, sizeof h);
    }
    e |= put_items_rec(f, JBP_R_READS, r->reads, r->nreads, 1);
    e |= put_items_rec(f, JBP_R_EXPECTS, r->expects, r->nexpects, JBP_EXPECT_WORDS);
    e |= put_items_rec(f, JBP_R_WAITS, r->waits, r->nwaits, JBP_WAITFAIL_WORDS);
    for (y = 0; y + 1 < (int)r->next_row; y += 2) {
        int x, k;
        jbp_put32(b, (uint32_t)y);
        jbp_put32(b + 4, 2);
        for (k = 0; k < 2; ++k)
            for (x = 0; x < JBP_VRAM_W; ++x) {
                uint16_t v = r->vram[(size_t)(y + k) * JBP_VRAM_W + x];
                b[8 + 2 * (k * JBP_VRAM_W + x)] = (uint8_t)v;
                b[9 + 2 * (k * JBP_VRAM_W + x)] = (uint8_t)(v >> 8);
            }
        e |= put_record(f, JBP_R_VRAM, b, sizeof b);
    }
    if (r->have_done) {
        jbp_put32(b, r->done_crc);
        jbp_put32(b + 4, r->done_frames);
        jbp_put32(b + 8, r->done_resent);
        jbp_put32(b + 12, r->done_rx_bad);
        jbp_put32(b + 16, r->done_sio_errors);
        e |= put_record(f, JBP_R_DONE, b, JBP_DONE_BYTES);
    }
    if (r->have_error) {
        for (i = 0; i < 3; ++i)
            jbp_put32(b + 4 * i, r->error[i]);
        e |= put_record(f, JBP_R_ERROR, b, JBP_ERROR_BYTES);
    }
    e |= fclose(f) != 0;
    if (e) {
        snprintf(err, size, "error writing %s", path);
        return -1;
    }
    return 0;
}

int jbp_result_parse(jbp_result *r, const uint8_t *b, size_t n, char *err, size_t size)
{
    size_t at = 12;

    if (jbp_result_init(r) != 0) {
        snprintf(err, size, "out of memory");
        jbp_result_free(r);
        return -1;
    }
    if (n < 12 || memcmp(b, file_magic, 8) != 0 || jbp_get32(b + 8) != FILE_VERSION) {
        snprintf(err, size, "not a probe result of version %u (magic or version)", FILE_VERSION);
        jbp_result_free(r);
        return -1;
    }
    while (at < n) {
        size_t rest = n - at, len, pad;
        unsigned type;
        /* Each length is compared with what is left before anything is
           added to it: no sum can wrap, whatever size_t is. */
        if (rest < 8)
            goto bad;
        type = jbp_get32(b + at);
        len = jbp_get32(b + at + 4);
        rest -= 8;
        if (len > rest)
            goto bad;
        pad = (4 - len % 4) % 4;
        if (pad > rest - len)
            goto bad;
        if (jbp_result_apply(r, type, b + at + 8, len) != 0)
            goto bad;
        at += 8 + len + pad;
    }
    return 0;
bad:
    snprintf(err, size, "a record is cut short or malformed (at byte %lu)", (unsigned long)at);
    jbp_result_free(r);
    return -1;
}

int jbp_result_load(jbp_result *r, const char *path, char *err, size_t size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long len = -1;
    int rc;

    memset(r, 0, sizeof *r);
    if (f == NULL) {
        snprintf(err, size, "cannot open %s", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) == 0)
        len = ftell(f);
    if (len < 0 || (unsigned long)len > FILE_MAX || fseek(f, 0, SEEK_SET) != 0 ||
        (b = malloc((size_t)len + 1)) == NULL || fread(b, 1, (size_t)len, f) != (size_t)len) {
        snprintf(err, size, "%s: cannot read it, or larger than any result (%lu bytes)", path,
                 (unsigned long)FILE_MAX);
        free(b);
        fclose(f);
        return -1;
    }
    fclose(f);
    rc = jbp_result_parse(r, b, (size_t)len, err, size);
    free(b);
    if (rc != 0) {
        char why[256];
        snprintf(why, sizeof why, "%s", err);
        snprintf(err, size, "%s: %s", path, why);
    }
    return rc;
}

/* ------------------------------------------------------------------------ */

static void text(FILE *f, const char *label, const uint8_t *p, size_t n)
{
    size_t i;
    int sep = 0;

    /* Strings separated by zero bytes; printable ASCII only. */
    fprintf(f, "%s: \"", label);
    for (i = 0; i < n; ++i) {
        if (p[i] == 0) {
            sep = 1;
            continue;
        }
        if (sep)
            fputs("\" \"", f);
        sep = 0;
        fputc(p[i] >= 32 && p[i] < 127 ? p[i] : '?', f);
    }
    fputs("\"\n", f);
}

void jbp_result_describe(const jbp_result *r, FILE *f)
{
    if (r->list_name[0])
        fprintf(f, "list: %s, %u bytes, CRC-32 0x%08x\n", r->list_name, (unsigned)r->list_bytes,
                (unsigned)r->list_crc);
    if (r->note[0])
        fprintf(f, "recorded: %s\n", r->note);
    if (r->have_hello) {
        const uint8_t *h = r->hello;
        uint32_t w[JBP_ID_WORDS];
        unsigned i;
        for (i = 0; i < JBP_ID_WORDS; ++i)
            w[i] = jbp_get32(h + 4 * i);
        fprintf(f, "platform: %s\n", w[JBP_ID_PLATFORM] == 1 ? "console" : w[JBP_ID_PLATFORM] == 2
                                                                         ? "host simulation (jitterbug)"
                                                                         : "unknown");
        fprintf(f, "CPU PRId: 0x%08x\n", (unsigned)w[JBP_ID_CPU_PRID]);
        fprintf(f, "GPU: v%u (GP1(10h) index 7: 0x%08x; index 2-5: 0x%08x 0x%08x 0x%08x 0x%08x; "
                   "index 8: 0x%08x)\n",
                (unsigned)w[JBP_ID_GPU_VERSION], (unsigned)w[JBP_ID_GP1_10_7],
                (unsigned)w[JBP_ID_GP1_10_2], (unsigned)w[JBP_ID_GP1_10_3],
                (unsigned)w[JBP_ID_GP1_10_4], (unsigned)w[JBP_ID_GP1_10_5],
                (unsigned)w[JBP_ID_GP1_10_8]);
        fprintf(f, "GPUSTAT at boot: 0x%08x (video %s as the BIOS left it)\n",
                (unsigned)w[JBP_ID_GPUSTAT_BOOT], (w[JBP_ID_GPUSTAT_BOOT] >> 20 & 1) ? "PAL" : "NTSC");
        fprintf(f, "BIOS date: %08x, console type word: 0x%08x\n", (unsigned)w[JBP_ID_BIOS_DATE],
                (unsigned)w[JBP_ID_CONSOLE_TYPE]);
        text(f, "BIOS kernel strings", h + 4 * JBP_ID_WORDS, JBP_ID_MAKER_BYTES);
        text(f, "BIOS version string", h + 4 * JBP_ID_WORDS + JBP_ID_MAKER_BYTES,
             JBP_ID_VERSION_BYTES);
        fprintf(f, "serial: factor %u, reload %u\n", (unsigned)w[JBP_ID_SIO_FACTOR],
                (unsigned)w[JBP_ID_SIO_RELOAD]);
    }
    if (r->have_head) {
        const uint32_t *d = r->head;
        fprintf(f, "status: %u; list %u bytes, CRC-32 0x%08x, %u ops before the main loop\n",
                (unsigned)d[JBP_H_STATUS], (unsigned)d[JBP_H_LIST_BYTES], (unsigned)d[JBP_H_LIST_CRC],
                (unsigned)d[JBP_H_OPS]);
        fprintf(f, "reads: %u (%u kept); checks: %u, %u differ from the list's\n",
                (unsigned)d[JBP_H_READS], (unsigned)d[JBP_H_READS_KEPT], (unsigned)d[JBP_H_EXPECTS],
                (unsigned)d[JBP_H_EXPECTS_FAILED]);
        fprintf(f, "waits: %u, %u timed out; DMA: %u, %u timed out; vsyncs: %u, %u timed out\n",
                (unsigned)d[JBP_H_WAITS], (unsigned)d[JBP_H_WAITS_FAILED], (unsigned)d[JBP_H_DMA_OPS],
                (unsigned)d[JBP_H_DMA_TIMEOUTS], (unsigned)d[JBP_H_VSYNCS],
                (unsigned)d[JBP_H_VSYNC_TIMEOUTS]);
        if (d[JBP_H_LOOP]) {
            fprintf(f, "main loop: compared after pass %u, then %u more played (window %u): ",
                    (unsigned)d[JBP_H_PASSES], (unsigned)(d[JBP_H_PLAYED] - d[JBP_H_PASSES]),
                    (unsigned)d[JBP_H_WINDOW]);
            if (!d[JBP_H_STEADY])
                fprintf(f, "pass %u CHANGED VRAM", (unsigned)d[JBP_H_CHANGED_AT]);
            else
                fprintf(f, "VRAM unchanged");
            if (d[JBP_H_EARLY])
                fprintf(f, "; pass %u changed nothing (EARLY)", (unsigned)d[JBP_H_PASSES]);
            fprintf(f, "; CRC-32 after pass %u 0x%08x\n", (unsigned)d[JBP_H_PASSES],
                    (unsigned)d[JBP_H_CRC_AT]);
        }
        fprintf(f, "GPUSTAT after the list: 0x%08x; before the read-back: 0x%08x (restore 0x%x)\n",
                (unsigned)d[JBP_H_GPUSTAT_END], (unsigned)d[JBP_H_GPUSTAT_READ],
                (unsigned)d[JBP_H_RESTORE]);
        fprintf(f, "serial: %u line errors, %u damaged frames received, %u frames sent again\n",
                (unsigned)d[JBP_H_SIO_ERRORS], (unsigned)d[JBP_H_RX_BAD], (unsigned)d[JBP_H_TX_RESENT]);
        fprintf(f, "time: list %u ms, main loop %u ms\n", (unsigned)d[JBP_H_LIST_MS],
                (unsigned)d[JBP_H_LOOP_MS]);
    }
    if (r->have_done)
        fprintf(f, "at the end: %u frames sent again, %u damaged frames received, %u line errors\n",
                (unsigned)r->done_resent, (unsigned)r->done_rx_bad, (unsigned)r->done_sio_errors);
    if (r->have_error)
        fprintf(f, "error: code %u (%s) at word %u, %u bytes received\n", (unsigned)r->error[0],
                jbp_error_name(r->error[0]), (unsigned)r->error[1], (unsigned)r->error[2]);
}

void jbp_list_name(const uint8_t *list, size_t n, char *buf, size_t size)
{
    size_t len, i;

    buf[0] = 0;
    if (n < 20 || size == 0)
        return;
    len = jbp_get32(list + 16);
    for (i = 0; i < len && i + 1 < size && 20 + i < n; ++i)
        buf[i] = (char)list[20 + i];
    buf[i] = 0;
}

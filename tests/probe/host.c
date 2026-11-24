/*
 * probe_host: the host's side of the probe (tools/probe/host: jbh_session.c,
 * jbp_result.c) against frames and result files made by hand and at random
 * (tests/CMakeLists.txt, "Hardware probe"). No probe, no GPU: what the
 * host is given here is what a broken or hostile probe, line or file could
 * give it.
 *
 *   probe_host --frames
 *       control frames of the wrong size or type, results of another list,
 *       and a probe that never lets the line go idle
 *   probe_host --files RESULT.jbpr
 *       RESULT (a complete simulated result with reads and checks) taken
 *       apart and put together wrongly: every record length that would wrap
 *       a 32-bit size, records cut, repeated, missing, overlapping, out of
 *       order, of the wrong size, strings without their zero; each must be
 *       refused
 *   probe_host --fuzz SEED ROUNDS RESULT.jbpr
 *       RESULT as frames to a session and as a file, damaged at random:
 *       the host must stay inside its memory and accept nothing that is not
 *       complete and of the list it sent; a round now and then with a probe
 *       that floods the line, which must not hold the host past its limits
 *
 * Exit status 0 when everything holds.
 */
#include "jbh_session.h"
#include "jbp_result.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long len;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (b = malloc((size_t)len + 1)) != NULL && fread(b, 1, (size_t)len, f) == (size_t)len)
        *n = (size_t)len;
    else {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

/* ------------------------------------------------------------------------ */
/* Result files as records. */

typedef struct rec {
    uint32_t type, len;
    const uint8_t *p;
} rec;

typedef struct recs {
    rec r[600];
    size_t n;
} recs;

static int split(const uint8_t *b, size_t n, recs *out)
{
    size_t at = 12;

    out->n = 0;
    while (at + 8 <= n && out->n < sizeof out->r / sizeof out->r[0]) {
        rec *r = &out->r[out->n++];
        r->type = jbp_get32(b + at);
        r->len = jbp_get32(b + at + 4);
        r->p = b + at + 8;
        at += 8 + ((r->len + 3u) & ~3u);
    }
    return at == n ? 0 : -1;
}

/* A growing byte buffer. */
typedef struct buf {
    uint8_t *b;
    size_t n, cap;
} buf;

static void put(buf *o, const void *p, size_t n)
{
    if (o->n + n > o->cap) {
        size_t cap = (o->n + n) * 2 + 64;
        uint8_t *more = realloc(o->b, cap);
        if (more == NULL)
            exit(2);
        o->b = more;
        o->cap = cap;
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}

static void put32(buf *o, uint32_t v)
{
    uint8_t b[4];

    jbp_put32(b, v);
    put(o, b, 4);
}

static void put_rec(buf *o, uint32_t type, uint32_t len, const uint8_t *p, size_t have)
{
    static const uint8_t zero[4] = {0, 0, 0, 0};

    put32(o, type);
    put32(o, len);
    put(o, p, have);
    put(o, zero, (4 - have % 4) % 4);
}

static void file_head(buf *o)
{
    o->n = 0;
    put(o, "JBPROBE", 8);
    put32(o, 4);
}

/* What parse and check make of bytes: 2 malformed, 1 parsed and not
   usable, 0 usable. */
static int verdict(const buf *o, char *why, size_t size)
{
    jbp_result r;
    int v;

    if (jbp_result_parse(&r, o->b, o->n, why, size) != 0)
        return 2;
    v = jbp_result_check(&r, why, size) != 0;
    jbp_result_free(&r);
    return v;
}

static void expect_verdict(const buf *o, int want, const char *what)
{
    char why[300] = "", line[512];
    int got = verdict(o, why, sizeof why);
    static const char *names[] = {"usable", "not usable", "malformed"};

    snprintf(line, sizeof line, "%s: %s%s%s (wanted %s)", what, names[got], why[0] ? ": " : "", why,
             names[want]);
    check(want == 2 ? got == 2 : want == 1 ? got >= 1 : got == 0, line);
}

/* The file rebuilt from its records, with record k changed by `how`. */
enum { KEEP, DROP, TWICE, ADD_WORD, CUT_WORD };

static void rebuild(buf *o, const recs *rs, size_t k, int how, uint32_t only_type)
{
    size_t i;
    static const uint8_t junk[4] = {1, 2, 3, 4};

    file_head(o);
    for (i = 0; i < rs->n; ++i) {
        const rec *r = &rs->r[i];
        int hit = only_type ? r->type == only_type : i == k;
        if (!hit || how == KEEP) {
            put_rec(o, r->type, r->len, r->p, r->len);
            continue;
        }
        switch (how) {
        case DROP:
            break;
        case TWICE:
            put_rec(o, r->type, r->len, r->p, r->len);
            put_rec(o, r->type, r->len, r->p, r->len);
            break;
        case ADD_WORD: {
            buf t = {NULL, 0, 0};
            put(&t, r->p, r->len);
            put(&t, junk, 4);
            put_rec(o, r->type, r->len + 4, t.b, t.n);
            free(t.b);
            break;
        }
        case CUT_WORD:
            if (r->len >= 4)
                put_rec(o, r->type, r->len - 4, r->p, r->len - 4);
            break;
        }
    }
}

static int files(const char *path)
{
    size_t n = 0, k;
    uint8_t *b = read_file(path, &n);
    recs rs;
    buf o = {NULL, 0, 0};
    char what[128];
    static const uint32_t types[] = {JBP_REC_HOST, JBP_HELLO, JBP_R_IDENT, JBP_R_HEAD, JBP_R_READS,
                                     JBP_R_EXPECTS, JBP_R_WAITS, JBP_R_VRAM, JBP_R_DONE, JBP_R_ERROR,
                                     0x7fu};
    uint32_t len;

    if (b == NULL || split(b, n, &rs) != 0) {
        printf("FAIL: cannot read %s as records\n", path);
        return 1;
    }
    rebuild(&o, &rs, 0, KEEP, 0);
    expect_verdict(&o, 0, "the file as it is");
    if (verdict(&o, what, sizeof what) != 0) {
        free(o.b);
        free(b);
        return 1;
    }
    {
        /* The file must have reads and checks, or the cases below about
           them prove nothing. */
        int have_r = 0, have_e = 0;
        for (k = 0; k < rs.n; ++k) {
            have_r |= rs.r[k].type == JBP_R_READS;
            have_e |= rs.r[k].type == JBP_R_EXPECTS;
        }
        check(have_r && have_e, "the file has reads and checks");
    }

    /* A length near 2^32 for every type, after a good header, on
       its own (EOF next) and with bytes after it. */
    for (k = 0; k < sizeof types / sizeof types[0]; ++k)
        for (len = 0xfffffff8u; len != 0; ++len) {
            file_head(&o);
            put32(&o, types[k]);
            put32(&o, len);
            if (len & 1)
                put(&o, b + 12, 64);
            snprintf(what, sizeof what, "type 0x%02x, length 0x%08x", (unsigned)types[k], (unsigned)len);
            if (verdict(&o, what, sizeof what) != 2) {
                printf("FAIL type 0x%02x, length 0x%08x: not refused\n", (unsigned)types[k], (unsigned)len);
                ++failures;
            }
        }
    check(1, "lengths 0xfffffff8 to 0xffffffff of every record type: done");
    /* A length past the end, and a record cut in its padding. */
    file_head(&o);
    put32(&o, JBP_REC_HOST);
    put32(&o, 64);
    put(&o, "x", 1);
    expect_verdict(&o, 2, "a record of 64 bytes with 1 there");
    o.n = 0;
    put(&o, b, n - 2);
    expect_verdict(&o, 2, "the file less its last 2 bytes");
    o.n = 0;
    put(&o, b, n);
    put(&o, "\0\0\0", 3);
    expect_verdict(&o, 2, "the file with 3 bytes more");
    o.n = 0;
    put(&o, b, 12);
    o.b[8] = 2;
    expect_verdict(&o, 2, "version 2");
    o.n = 0;
    put(&o, b, n);
    o.b[8] = 3;
    expect_verdict(&o, 2, "the whole file as version 3 (protocol 3)");

    /* Every record dropped, doubled, a word longer, a word shorter;
       every kind dropped as a whole. Each is refused. */
    for (k = 0; k < rs.n; ++k) {
        static const char *how_name[] = {"", "dropped", "twice", "a word longer", "a word shorter"};
        int how;
        for (how = DROP; how <= CUT_WORD; ++how) {
            rebuild(&o, &rs, k, how, 0);
            snprintf(what, sizeof what, "record %u (type 0x%02x) %s", (unsigned)k, (unsigned)rs.r[k].type,
                     how_name[how]);
            if (verdict(&o, what, sizeof what) == 0) {
                printf("FAIL %s: still usable\n", what);
                ++failures;
            }
        }
    }
    check(1, "every record dropped, doubled, longer and shorter: done");
    for (k = 0; k < sizeof types / sizeof types[0]; ++k) {
        int present = 0;
        size_t i;
        for (i = 0; i < rs.n; ++i)
            present |= rs.r[i].type == types[k];
        if (!present)
            continue;
        rebuild(&o, &rs, 0, DROP, types[k]);
        snprintf(what, sizeof what, "every record of type 0x%02x dropped", (unsigned)types[k]);
        expect_verdict(&o, 1, what);
    }

    /* Hand-made wrong records, each in place of the file's own. */
    for (k = 0; k < rs.n; ++k) {
        const rec *r = &rs.r[k];
        buf t = {NULL, 0, 0};
        size_t i;
        put(&t, r->p, r->len);
        if (r->type == JBP_REC_HOST) {
            /* The note without its zero, and with one inside. */
            t.b[t.n - 1] = 'x';
        } else if (r->type == JBP_R_READS || r->type == JBP_R_EXPECTS) {
            jbp_put32(t.b, 1); /* starts at an index already had: overlaps */
        } else if (r->type == JBP_R_VRAM && jbp_get32(r->p) == 2) {
            jbp_put32(t.b, 0); /* rows 0 and 1 again, in place of 2 and 3 */
        } else if (r->type == JBP_R_HEAD) {
            jbp_put32(t.b + 4 * JBP_H_READS, jbp_get32(r->p + 4 * JBP_H_READS) + 1);
            jbp_put32(t.b + 4 * JBP_H_READS_KEPT, jbp_get32(r->p + 4 * JBP_H_READS_KEPT) + 1);
        } else {
            free(t.b);
            continue;
        }
        file_head(&o);
        for (i = 0; i < rs.n; ++i)
            if (i == k)
                put_rec(&o, r->type, (uint32_t)t.n, t.b, t.n);
            else
                put_rec(&o, rs.r[i].type, rs.r[i].len, rs.r[i].p, rs.r[i].len);
        free(t.b);
        snprintf(what, sizeof what, "record %u (type 0x%02x) made wrong by hand", (unsigned)k,
                 (unsigned)r->type);
        if (verdict(&o, what, sizeof what) == 0) {
            printf("FAIL %s: still usable\n", what);
            ++failures;
        }
    }
    check(1, "hand-made wrong records: done");

    /* The summary of another list than the host's record names (in
       a file). */
    {
        jbp_result r;
        char err[300];
        if (jbp_result_parse(&r, b, n, err, sizeof err) == 0) {
            const char *tmp = "probe_host_other.jbpr";
            size_t m = 0;
            uint8_t *c;
            r.head[JBP_H_LIST_CRC] ^= 1;
            if (jbp_result_save(&r, tmp, err, sizeof err) == 0 && (c = read_file(tmp, &m)) != NULL) {
                buf t = {c, m, m};
                expect_verdict(&t, 1, "a summary of another list than the host record's");
                free(c);
            } else
                check(0, err);
            remove(tmp);
            jbp_result_free(&r);
        }
    }
    /* The waits that timed out: a result with one, sound; then without
       its R_WAITS record; then with the count above what was sent. */
    {
        jbp_result r;
        char err[300];
        if (jbp_result_parse(&r, b, n, err, sizeof err) == 0) {
            const char *tmp = "probe_host_waits.jbpr";
            static const uint32_t w[JBP_WAITFAIL_WORDS] = {100, 0x10000000u, 0x10000000u, 0, 1};
            size_t m = 0;
            uint8_t *c;
            recs ws;
            r.waits = malloc(sizeof w);
            if (r.waits == NULL)
                exit(2);
            memcpy(r.waits, w, sizeof w);
            r.nwaits = 1;
            r.head[JBP_H_WAITS] += 1;
            r.head[JBP_H_WAITS_FAILED] = 1;
            if (jbp_result_save(&r, tmp, err, sizeof err) == 0 && (c = read_file(tmp, &m)) != NULL &&
                split(c, m, &ws) == 0) {
                buf t = {NULL, 0, 0};
                rebuild(&t, &ws, 0, KEEP, 0);
                expect_verdict(&t, 0, "a result with a wait that timed out");
                rebuild(&t, &ws, 0, DROP, JBP_R_WAITS);
                expect_verdict(&t, 1, "the same without its R_WAITS record");
                free(t.b);
                free(c);
            } else
                check(0, "cannot write a result with a wait");
            r.head[JBP_H_WAITS_FAILED] = JBP_WAITFAIL_KEPT + 1;
            r.head[JBP_H_WAITS] = JBP_WAITFAIL_KEPT + 1;
            if (jbp_result_save(&r, tmp, err, sizeof err) == 0 && (c = read_file(tmp, &m)) != NULL) {
                buf t = {c, m, m};
                expect_verdict(&t, 1, "65 waits timed out, 1 sent (the first 64 are)");
                free(c);
            }
            remove(tmp);
            jbp_result_free(&r);
        }
    }
    /* Retention: a result with more reads than the probe keeps. */
    {
        uint8_t big[4 + 4 * 1025];
        size_t i, chunks = JBP_READS_MAX / 1024 + 1;
        file_head(&o);
        for (i = 0; i < chunks; ++i) {
            memset(big, 0, sizeof big);
            jbp_put32(big, (uint32_t)(i * 1024));
            put_rec(&o, JBP_R_READS, 4 + 4 * 1024, big, 4 + 4 * 1024);
        }
        expect_verdict(&o, 2, "17 parts of 1024 reads: more than the probe keeps");
    }
    /* A file that does not exist, and one that is not a result, through
       jbp_result_load (which frees what it built: LeakSanitizer checks). */
    {
        jbp_result r;
        char err[300];
        check(jbp_result_load(&r, "no/such/file.jbpr", err, sizeof err) != 0, "no such file: refused");
        check(jbp_result_load(&r, __FILE__, err, sizeof err) != 0, "a C file: refused");
    }
    free(o.b);
    free(b);
    printf("%s\n", failures ? "FAIL" : "OK");
    return failures != 0;
}

/* ------------------------------------------------------------------------ */
/* Sessions. */

static buf sent; /* what the host wrote */

static void host_write(void *user, const uint8_t *p, size_t n)
{
    (void)user;
    put(&sent, p, n);
}

static void feed(jbh_session *s, unsigned type, unsigned seq, const void *p, size_t n, uint32_t now)
{
    uint8_t f[JBP_MAX_FRAME];
    size_t len = jbp_frame(f, type, seq, p, n);

    jbh_input(s, f, len, now);
}

static const uint8_t the_list[28] = {'J', 'B', 'C', 'L', 'I', 'S', 'T', 0, 1, 0, 0, 0, 24, 0,
                                     0,   0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0,  0};

static void begin(jbh_session *s, uint32_t now)
{
    jbh_options o;

    jbh_defaults(&o);
    o.write = host_write;
    sent.n = 0;
    if (jbh_begin(s, the_list, sizeof the_list, &o, now) != 0)
        exit(2);
}

/* Through START and the one LIST frame, to the results. */
static void to_results(jbh_session *s)
{
    begin(s, 0);
    feed(s, JBP_ACK, 0, NULL, 0, 1);
    feed(s, JBP_ACK, 1, NULL, 0, 2);
}

/* A probe (or a line) that never lets the host's line go idle, with
   frames the host does not act on or acts on without progress. Each step
   puts one frame or run of bytes on the line and polls; the session must
   fail within its own limit (start_ms, tries * ack_ms, quiet_ms; 60 s
   each by default), however fast the frames come. `phase`: 0 START sent,
   1 the list being sent, 2 waiting for the results. */
enum { HF_HELLO, HF_NAK, HF_DAMAGED, HF_IGNORED };

static uint32_t rnd(uint32_t n);

/* The flood on s, from its phase: when the session failed (or 200000 ms
   when it did not). kind -1: one at random each step; step 0: 1 to 1000
   ms at random. */
static uint32_t flood_run(jbh_session *s, int phase, int kind0, uint32_t step0)
{
    uint8_t hello[JBP_HELLO_BYTES], p[8], bad[16];
    uint32_t t = 2, step;
    unsigned long k;
    int kind;

    memset(hello, 0, sizeof hello);
    jbp_put32(hello + 4 * JBP_ID_PROTOCOL, JBP_PROTOCOL_VERSION);
    memset(p, 0x5a, sizeof p);
    if (phase == 2)
        to_results(s);
    else {
        begin(s, 0);
        if (phase == 1)
            feed(s, JBP_ACK, 0, NULL, 0, 1);
    }
    for (k = 0; s->state == JBH_RUNNING && t < 200000; ++k, t += step) {
        kind = kind0 >= 0 ? kind0 : (int)rnd(4);
        step = step0 ? step0 : 1 + rnd(1000);
        switch (kind) {
        case HF_HELLO: feed(s, JBP_HELLO, 0, hello, sizeof hello, t); break;
        case HF_NAK: feed(s, JBP_NAK, 0, NULL, 0, t); break;
        case HF_DAMAGED:
            memcpy(bad, "JB\xff\x00\x04\x00\x5a\x5a\x5a\x5a\x00\x00\x00\x00", 14); /* a wrong CRC-32 */
            jbh_input(s, bad, 14, t);
            break;
        default:
            /* In turn: no type, an ACK of another seq, a NAK with a payload,
               a result before its time or of another seq, the host's own
               START. */
            switch (k % 5) {
            case 0: feed(s, 0xff, 0, p, 4, t); break;
            case 1: feed(s, JBP_ACK, (s->seq + 128) & 0xffu, NULL, 0, t); break;
            case 2: feed(s, JBP_NAK, 0, p, 1, t); break;
            case 3: feed(s, JBP_R_DONE, 200, p, 8, t); break;
            default: feed(s, JBP_START, 0, p, 8, t); break;
            }
            break;
        }
        jbh_poll(s, t);
        if (s->state != JBH_RUNNING)
            break; /* t: when it failed */
    }
    return s->state == JBH_FAILED ? t : 200000;
}

static void flood_case(int phase, int kind, uint32_t step, uint32_t latest, const char *what)
{
    static jbh_session s;
    uint32_t t = flood_run(&s, phase, kind, step);

    check(s.state == JBH_FAILED && t <= latest, what);
    printf("     (after %u ms: %s)\n", (unsigned)t, s.state == JBH_FAILED ? s.why : "still running");
    jbh_end(&s);
}

static void flood_cases(void)
{
    flood_case(0, HF_HELLO, 1000, 61000, "an idle probe's HELLOs, its START never taken: the host gives up");
    flood_case(0, HF_HELLO, 10, 61000, "HELLOs every 10 ms before the START was taken: the host gives up");
    flood_case(0, HF_NAK, 10, 61000, "NAKs every 10 ms before the START was taken: the host gives up");
    flood_case(0, HF_DAMAGED, 10, 61000, "damaged frames every 10 ms before the START was taken: the host gives up");
    flood_case(0, HF_IGNORED, 1, 61000, "ignored frames every ms before the START was taken: the host gives up");
    flood_case(1, HF_NAK, 10, 49000, "NAKs every 10 ms while the list is sent: the host gives up");
    flood_case(1, HF_DAMAGED, 10, 49000, "damaged frames every 10 ms while the list is sent: the host gives up");
    flood_case(1, HF_IGNORED, 1, 49000, "ignored frames every ms while the list is sent: the host gives up");
    flood_case(2, HF_DAMAGED, 10, 61000, "damaged frames every 10 ms (NAKed) awaiting the results: the host gives up");
    flood_case(2, HF_IGNORED, 1, 61000, "ignored frames every ms awaiting the results: the host gives up");
}

static int frames(void)
{
    static jbh_session s;
    uint8_t hello[JBP_HELLO_BYTES], head[4 * JBP_H_WORDS], prog[JBP_PROGRESS_BYTES];
    size_t before;

    memset(hello, 0, sizeof hello);
    jbp_put32(hello + 4 * JBP_ID_PROTOCOL, JBP_PROTOCOL_VERSION);

    begin(&s, 1);
    feed(&s, 0xff, 0, NULL, 0, 8);
    check(s.heard_ms == 1, "an unknown frame type is not hearing from the probe");
    feed(&s, JBP_START, 0, NULL, 0, 9);
    check(s.heard_ms == 1, "a START (the host's own type) is not hearing from the probe");
    feed(&s, JBP_ACK, 0, "x", 1, 10);
    check(s.seq == 0 && s.heard_ms == 1, "an ACK with a payload does not advance the host");
    before = sent.n;
    feed(&s, JBP_NAK, 0, "x", 1, 11);
    check(sent.n == before && s.resent == 0, "a NAK with a payload does not make it send again");
    feed(&s, JBP_HELLO, 0, hello, sizeof hello - 1, 12);
    check(s.heard_ms == 1, "a short HELLO is not hearing from the probe");
    jbp_put32(hello + 4 * JBP_ID_PROTOCOL, 2);
    feed(&s, JBP_HELLO, 0, hello, sizeof hello, 13);
    check(s.heard_ms == 1 && !s.result.have_hello, "a HELLO of protocol 2 is not taken");
    jbp_put32(hello + 4 * JBP_ID_PROTOCOL, JBP_PROTOCOL_VERSION);
    jbp_put32(prog, 7);
    feed(&s, JBP_PROGRESS, 0, prog, 4, 14);
    check(s.heard_ms == 1 && s.sent_ms == 1, "a short PROGRESS does not hold the host off");
    jbp_put32(prog, 9);
    jbp_put32(prog + 4, 0);
    jbp_put32(prog + 8, 0);
    feed(&s, JBP_PROGRESS, 0, prog, sizeof prog, 15);
    check(s.heard_ms == 1 && s.sent_ms == 1, "a PROGRESS of no phase does not hold the host off");
    jbp_put32(prog, JBP_P_LOOP);
    feed(&s, JBP_PROGRESS, 0, prog, sizeof prog, 16);
    check(s.heard_ms == 16 && s.sent_ms == 16, "a good PROGRESS holds the host off");
    feed(&s, JBP_ACK, 0, NULL, 0, 17);
    check(s.seq == 1 && s.heard_ms == 17, "a good ACK advances the host");
    feed(&s, JBP_R_HEAD, 0, head, sizeof head, 18);
    check(s.heard_ms == 17 && s.result.have_head == 0 && s.state == JBH_RUNNING,
          "a summary before the list was sent is not taken");
    jbh_end(&s);

    /* Results of another list complete nothing. */
    to_results(&s);
    feed(&s, JBP_R_IDENT, 0, hello, sizeof hello, 20);
    memset(head, 0, sizeof head);
    jbp_put32(head + 4 * JBP_H_LIST_BYTES, 1972);
    jbp_put32(head + 4 * JBP_H_LIST_CRC, 0x9259a588u);
    feed(&s, JBP_R_HEAD, 1, head, sizeof head, 21);
    check(s.state == JBH_FAILED && strstr(s.why, "not the 28 bytes") != NULL,
          "a summary of another list fails the session");
    printf("     (%s)\n", s.why);
    jbh_end(&s);
    to_results(&s);
    feed(&s, JBP_R_IDENT, 0, hello, sizeof hello, 20);
    jbp_put32(head + 4 * JBP_H_LIST_BYTES, sizeof the_list);
    jbp_put32(head + 4 * JBP_H_LIST_CRC, jbp_crc32(0, the_list, sizeof the_list));
    feed(&s, JBP_R_HEAD, 1, head, sizeof head, 21);
    check(s.state == JBH_RUNNING && s.result.have_head, "a summary of the list sent is taken");
    {
        /* A frame with the last result's seq is that frame sent
           again only when it is equal to it; anything else with that seq
           is neither acknowledged nor heard. */
        static uint8_t big[JBP_MAX_PAYLOAD];
        uint32_t heard = s.heard_ms;
        memcpy(big, head, sizeof head);
        before = sent.n;
        feed(&s, JBP_R_HEAD, 1, big, JBP_MAX_PAYLOAD, 30);
        check(sent.n == before && s.heard_ms == heard && s.state == JBH_RUNNING,
              "the last seq again with an impossible length: not acknowledged, not heard");
        feed(&s, JBP_R_DONE, 1, big, JBP_DONE_BYTES, 31);
        check(sent.n == before && s.heard_ms == heard && s.state == JBH_RUNNING && !s.result.have_done,
              "the last seq again as another type: not acknowledged, not heard, not applied");
        feed(&s, JBP_R_HEAD, 1, big, sizeof head - 4, 32);
        check(sent.n == before && s.heard_ms == heard, "the last seq again a word shorter: ignored");
        big[4] ^= 1;
        feed(&s, JBP_R_HEAD, 1, big, sizeof head, 33);
        check(sent.n == before && s.heard_ms == heard, "the last seq again with another payload: ignored");
        big[4] ^= 1;
        feed(&s, JBP_ACK, 5, NULL, 0, 34);
        feed(&s, JBP_NAK, 0, NULL, 0, 35);
        check(sent.n == before && s.heard_ms == heard && s.state == JBH_RUNNING,
              "an ACK or NAK with nothing outstanding is not hearing from the probe");
        feed(&s, JBP_R_HEAD, 1, big, sizeof head, 36);
        check(sent.n == before + JBP_HEADER_BYTES + JBP_TRAILER_BYTES && sent.b[before + 2] == JBP_ACK &&
                  sent.b[before + 3] == 1 && s.heard_ms == 36 && s.state == JBH_RUNNING,
              "the last frame sent again, equal: acknowledged again and heard");
    }
    feed(&s, JBP_R_HEAD, 2, head, sizeof head, 22);
    check(s.state == JBH_FAILED, "a second summary fails the session");
    jbh_end(&s);
    /* A HELLO once the START was taken: the probe is idle, its results
       cannot come; the session fails at once rather than being held off. */
    to_results(&s);
    feed(&s, JBP_HELLO, 0, hello, sizeof hello, 40);
    check(s.state == JBH_FAILED && s.heard_ms != 40 && strstr(s.why, "idle") != NULL,
          "a HELLO in the middle of the session fails it");
    jbh_end(&s);
    begin(&s, 1);
    feed(&s, JBP_HELLO, 0, hello, sizeof hello, 200);
    check(s.state == JBH_RUNNING && s.heard_ms == 200 && s.result.have_hello && s.resent == 1,
          "a HELLO before the START was taken: heard, and the START sent again");
    jbh_end(&s);
    flood_cases();
    free(sent.b);
    sent.b = NULL;
    sent.cap = 0;
    printf("%s\n", failures ? "FAIL" : "OK");
    return failures != 0;
}

/* ------------------------------------------------------------------------ */
/* Fuzz. */

static uint32_t fz;

static uint32_t rnd(uint32_t n)
{
    fz ^= fz << 13;
    fz ^= fz >> 17;
    fz ^= fz << 5;
    return n ? fz % n : 0;
}

static int fuzz(uint32_t seed, unsigned long rounds, const char *path)
{
    size_t n = 0, k;
    uint8_t *b = read_file(path, &n);
    recs rs;
    jbp_result good;
    char err[300];
    unsigned long done = 0, files_ok = 0, bad = 0, round, dup_bogus = 0, dup_exact = 0, flooded = 0;
    static uint8_t prevp[JBP_MAX_PAYLOAD + 16];
    size_t prevn = 0;
    unsigned prevt = 0;
    static jbh_session s;
    buf o = {NULL, 0, 0}, f = {NULL, 0, 0};

    fz = seed ? seed : 1;
    if (b == NULL || split(b, n, &rs) != 0 || jbp_result_parse(&good, b, n, err, sizeof err) != 0) {
        printf("FAIL: cannot read %s\n", path);
        return 1;
    }
    for (round = 0; round < rounds; ++round) {
        jbh_options opt;
        unsigned seq = 0;
        uint32_t now = 10, hits;
        size_t hit[3];
        uint8_t *list;
        if (rnd(20) == 0) {
            /* A probe that never lets the line go idle, from a
               random phase, with frames of every kind at random intervals:
               the session fails within its limits (60 s; 48 s for the
               list's frames), plus the last interval. */
            int phase = (int)rnd(3);
            uint32_t t = flood_run(&s, phase, -1, 0);
            if (s.state != JBH_FAILED || t > 61000) {
                printf("FAIL: round %lu (seed %u): a flooding probe (phase %d) held the host for %u ms\n", round,
                       (unsigned)seed, phase, (unsigned)t);
                ++bad;
            }
            jbh_end(&s);
            ++flooded;
            continue;
        }
        /* A session of the result's own list (as far as the host can
           tell: its length and CRC-32; its bytes are not needed here). */
        list = calloc(1, good.list_bytes ? good.list_bytes : 1);
        if (list == NULL)
            exit(2);
        jbh_defaults(&opt);
        opt.write = host_write;
        opt.passes = good.head[JBP_H_PASSES];
        sent.n = 0;
        if (jbh_begin(&s, list, good.list_bytes, &opt, 0) != 0)
            exit(2);
        s.result.list_crc = good.list_crc; /* as if the list's bytes were these */
        strcpy(s.result.list_name, good.list_name);
        feed(&s, JBP_ACK, 0, NULL, 0, 1);
        while (s.next < s.list_n && s.state == JBH_RUNNING)
            feed(&s, JBP_ACK, s.seq, NULL, 0, 2);
        /* The result's records as frames (R_IDENT for the identification),
           with 0 to 3 of them damaged (good CRC-32s), plus junk. */
        hits = rnd(4);
        for (k = 0; k < 3; ++k)
            hit[k] = hits > k ? rnd((uint32_t)rs.n) : rs.n;
        for (k = 0; k < rs.n && s.state == JBH_RUNNING; ++k) {
            const rec *r = &rs.r[k];
            uint8_t p[JBP_MAX_PAYLOAD + 16];
            size_t len = r->len < JBP_MAX_PAYLOAD ? r->len : JBP_MAX_PAYLOAD;
            unsigned type = r->type == JBP_HELLO ? JBP_R_IDENT : r->type, sq = seq;
            if (r->type == JBP_REC_HOST)
                continue;
            memcpy(p, r->p, len);
            switch (k == hit[0] || k == hit[1] || k == hit[2] ? rnd(7) : 7) {
            case 0: p[rnd((uint32_t)len)] ^= (uint8_t)(1u << rnd(8)); break;
            case 1: len = rnd((uint32_t)len + 8); break;
            case 2: type = rnd(256); break;
            case 3: sq = rnd(256); break;
            case 4: continue; /* lost */
            case 5: feed(&s, rnd(256), rnd(256), p, rnd(64), now); break;
            case 6: feed(&s, JBP_ACK, rnd(256), p, rnd(3), now); break;
            default: break;
            }
            if (s.have_last && s.state == JBH_RUNNING && rnd(6) == 0) {
                /* The last frame applied again, changed (type,
                   length or payload): neither acknowledged nor heard; or
                   unchanged: acknowledged again and heard. */
                static uint8_t q[JBP_MAX_PAYLOAD + 16];
                size_t qn = prevn, sent0 = sent.n;
                unsigned qt = prevt;
                uint32_t heard = s.heard_ms;
                int exact = rnd(3) == 0;
                memcpy(q, prevp, prevn);
                if (!exact) {
                    switch (rnd(3)) {
                    case 0: qt = JBP_R_HEAD + (prevt - JBP_R_HEAD + 1 + rnd(7)) % 8; break;
                    case 1: qn = (prevn + 1 + rnd(JBP_MAX_PAYLOAD)) % (JBP_MAX_PAYLOAD + 1); break;
                    default: q[rnd((uint32_t)prevn)] ^= (uint8_t)(1u << rnd(8)); break;
                    }
                }
                feed(&s, qt, s.rx_last, q, qn, now + 1);
                if (exact ? sent.n != sent0 + JBP_HEADER_BYTES + JBP_TRAILER_BYTES || s.heard_ms != now + 1
                          : sent.n != sent0 || s.heard_ms != heard || s.state != JBH_RUNNING) {
                    printf("FAIL: round %lu (seed %u): the last frame again (%s, type 0x%02x, %u bytes) was "
                           "%sacknowledged\n",
                           round, (unsigned)seed, exact ? "unchanged" : "changed", qt, (unsigned)qn,
                           sent.n == sent0 ? "not " : "");
                    ++bad;
                }
                dup_exact += exact;
                dup_bogus += !exact;
            }
            feed(&s, type, sq, p, len, now);
            if (s.have_last && s.rx_last == sq && s.last_type == type && s.last_len == len &&
                jbp_crc32(0, p, len) == s.last_crc) {
                memcpy(prevp, p, len);
                prevn = len;
                prevt = type;
            }
            if ((k == hit[0] || k == hit[1]) && rnd(4) == 0) {
                /* Raw bytes: a frame cut short, or noise. */
                uint8_t junk[32];
                size_t i;
                for (i = 0; i < sizeof junk; ++i)
                    junk[i] = (uint8_t)rnd(256);
                jbh_input(&s, junk, sizeof junk, now);
            }
            now += 1 + rnd(10);
            jbh_poll(&s, now);
            seq = (seq + 1) & 0xffu;
        }
        if (s.state == JBH_DONE) {
            char why[300];
            ++done;
            if (jbp_result_check(&s.result, why, sizeof why) != 0 ||
                s.result.head[JBP_H_LIST_BYTES] != good.list_bytes ||
                s.result.head[JBP_H_LIST_CRC] != good.list_crc) {
                printf("FAIL: round %lu (seed %u): a session completed with a result that is not sound\n",
                       round, (unsigned)seed);
                ++bad;
            }
        }
        jbh_end(&s);
        free(list);

        /* The file, damaged at random: a few bits, a few words, a cut. */
        f.n = 0;
        put(&f, b, n);
        switch (rnd(4)) {
        case 0:
            for (k = 1 + rnd(4); k > 0; --k)
                f.b[rnd((uint32_t)f.n)] ^= (uint8_t)(1u << rnd(8));
            break;
        case 1:
            for (k = 1 + rnd(3); k > 0; --k)
                jbp_put32(f.b + 4 * rnd((uint32_t)f.n / 4), rnd(4) ? fz : 0xffffffffu - rnd(4));
            break;
        case 2:
            f.n = rnd((uint32_t)f.n);
            break;
        default:
            /* A record's length word in particular. */
            if (rs.n > 0) {
                const rec *r = &rs.r[rnd((uint32_t)rs.n)];
                jbp_put32(f.b + (r->p - b) - 4, rnd(2) ? 0xffffffffu - rnd(8) : rnd(5000));
            }
            break;
        }
        {
            jbp_result r;
            if (jbp_result_parse(&r, f.b, f.n, err, sizeof err) == 0) {
                char why[300];
                if (jbp_result_check(&r, why, sizeof why) == 0) {
                    ++files_ok;
                    if (jbp_result_vram_crc(&r) != r.done_crc) {
                        printf("FAIL: round %lu: a damaged file accepted with a wrong VRAM\n", round);
                        ++bad;
                    }
                }
                jbp_result_free(&r);
            }
        }
    }
    jbp_result_free(&good);
    free(o.b);
    free(f.b);
    free(sent.b);
    free(b);
    printf("%lu rounds (seed %u): %lu sessions completed, %lu damaged files still usable (damage in a "
           "note or a count of the line); the last frame again %lu times changed, %lu unchanged; "
           "%lu sessions with a flooding probe; %lu failures\n%s\n",
           rounds, (unsigned)seed, done, files_ok, dup_bogus, dup_exact, flooded, bad, bad ? "FAIL" : "OK");
    if (done == 0 || dup_bogus == 0 || dup_exact == 0 || flooded == 0) {
        printf("FAIL: the fuzz did not reach every path (a completed session, the last frame again, a "
               "flooding probe)\n");
        return 1;
    }
    return bad != 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--frames") == 0)
        return frames();
    if (argc == 3 && strcmp(argv[1], "--files") == 0)
        return files(argv[2]);
    if (argc == 5 && strcmp(argv[1], "--fuzz") == 0)
        return fuzz((uint32_t)strtoul(argv[2], NULL, 10), strtoul(argv[3], NULL, 10), argv[4]);
    fprintf(stderr, "usage: probe_host --frames | --files RESULT.jbpr | --fuzz SEED ROUNDS RESULT.jbpr\n");
    return 2;
}

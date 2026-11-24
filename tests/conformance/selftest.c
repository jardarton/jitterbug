/*
 * conformance_selftest: the instrument checked against itself.
 *
 *   selftest --dump-list LIST DUMP --check-list LIST --fixtures TABLE
 *            --cut NAME LIST --exclusion-of NAME --scratch DIR --lists LIST...
 *
 * (DIR, which must exist, receives the tables of 8.)
 * (TABLE is fixtures.txt, from which the fixture NAME of --cut and the
 * exclusion of --exclusion-of's NAME are read, as the runner reads them).
 *
 * 1. A difference is seen. DUMP_LIST is played and compared with DUMP
 *    (D pixels differ). Then one pixel that matched is changed, once in a
 *    copy of the dump (its lowest colour bit flipped) and once in a copy of
 *    the list (a 1 x 1 GP0(A0h) transfer of another colour appended before
 *    END): each must give exactly D + 1, with that pixel in the box.
 * 2. A check that fails is seen. In a copy of CHECK_LIST, the console's
 *    value of the first check that passes is changed in one bit of its
 *    mask: one more check must fail.
 * 3. A list that is not the stated fixture is refused: the
 *    fixture list must pass jbc_check_fixture, and a copy cut after its
 *    first EXPECT (an END put there) must not.
 * 4. Only the documented exclusion is accepted: the exclusion file
 *    the table names must give the pixel count and checksum the table
 *    states, and an exclusion of all of VRAM ("rect 0 0 1024 512") must
 *    not.
 * 5. A main loop that changes VRAM or the GPU state after the stated pass
 *    is a difference:
 *    - late-loop: the loop draws a tile into an empty drawing area and
 *      then widens the area, so VRAM stays black after its first two
 *      passes and is drawn in the third. Played to pass 1 with the window
 *      it must be found unsettled, in its state at pass 2 (the drawing
 *      area) and in VRAM at pass 3, and judged DIFFERS against a black
 *      dump; played the old way (until a pass changes nothing) it stops at
 *      pass 1, which is what let it pass before.
 *    - gate-chain: VRAM never changes; pass 2 opens the GP1(09h) gate (not
 *      readable), so pass 3's GP0(E1h) stores its bit 11 and GPUSTAT bit 15
 *      changes there. The window must fail it at pass 3.
 *    - late-vram: a CPU-to-VRAM transfer open
 *      across LOOP, one payload word a pass. The reader must refuse it (the
 *      packet rule, FORMAT.md, "Packet boundaries at LOOP and END").
 *    - clut-chain: the limit FORMAT.md states. The loop carries a palette
 *      from pass to pass in the CLUT cache alone (drawn out, copied over its
 *      CLUT, loaded again, the VRAM put back), rotated by one entry each
 *      pass, and draws one pixel with it: VRAM after passes 1-4 is the
 *      same and pass 5 changes it. The window at pass 1 accepts it, since
 *      the cache cannot be read; played to pass 5 the window holds and
 *      pass 5 changed VRAM. If this ever fails, the state compared has
 *      grown and FORMAT.md is to be updated with it.
 * 6. The verdict: for an expected mismatch only a VRAM, check
 *    or read difference passes; a failed wait or an unsettled loop fails; a
 *    full match fails; a match with exclusions says how many pixels; a
 *    check the console of a probe result failed always fails.
 * 7. The result does not depend on how the words are cut or on threads.
 *    Every LIST is played with one jb_gp0 call per op on one thread, and
 *    again a word per call, in random chunks across ops (three seeds), and
 *    on four threads (by op, and in random chunks): VRAM, every value
 *    read, the main loop's passes and state, and every check and wait must
 *    be the same.
 * 8. A table is checked whole before a line is taken: TABLE with a
 *    second line for quad appended is refused when triangle is asked for,
 *    as are a table with a malformed line for another program (an
 *    exclusion missing a field), a dump line without its REFSUM, a probe
 *    line with one, and a log line with an exclusion.
 * 9. The packet rule (FORMAT.md, "Packet boundaries at LOOP and
 *    END"), on these
 *    lists: an odd upload with its rounded-up last word, a GP1(01h) and
 *    its mirrors 41h and 81h after a partial upload, both kinds of
 *    polyline ended after two vertices, GP0(1Fh) and GP0(03h) end between
 *    packets; an odd upload a word short, a partial upload followed by
 *    GP1(02h), and a shaded polyline whose terminator-like word is a
 *    colour do not.
 * 10. The window's report speaks only of the passes played: a loop
 *    whose pass 2 changes VRAM, compared at pass 1, stops there, and its
 *    report names pass 2 and not pass 3.
 */
#include "jbc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#if defined(__GNUC__)
static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

static void fail(const char *fmt, ...)
{
    va_list ap;

    ++failures;
    printf("FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int play(const jbc_list *l, int threads, jbc_split split, uint32_t seed, jbc_outcome *out)
{
    jbc_options o;
    char err[256];

    memset(&o, 0, sizeof o);
    o.threads = threads;
    o.split = split;
    o.seed = seed;
    if (jbc_play(l, &o, out, err, sizeof err) != 0) {
        printf("ERROR %s: %s\n", l->name, err);
        exit(2);
    }
    return 0;
}

static void load(const char *path, jbc_list *l)
{
    char err[256];

    if (jbc_load(path, l, err, sizeof err) != 0) {
        printf("ERROR %s: %s\n", path, err);
        exit(2);
    }
}

static void detect_pixel(const char *list_path, const char *dump_path)
{
    jbc_list l, l2;
    jbc_outcome base, changed;
    jbc_diff d, d2;
    const char *why = "";
    uint16_t *hw = jbc_load_dump(dump_path, &why), *hw2;
    size_t i, at = 0;
    int x, y;
    uint32_t words[5];

    if (hw == NULL) {
        printf("ERROR %s: %s\n", dump_path, why);
        exit(2);
    }
    load(list_path, &l);
    play(&l, 1, JBC_SPLIT_OP, 0, &base);
    jbc_compare(base.vram, hw, &d);
    printf("%s: %lu pixels differ from the dump\n", l.name, d.count);

    /* A matching pixel without the mask bit, from the middle of VRAM. */
    for (i = 0; i < JBC_VRAM_PIXELS; ++i) {
        at = (JBC_VRAM_PIXELS / 2 + 517 + i) % JBC_VRAM_PIXELS;
        if ((base.vram[at] & 0x7fffu) == hw[at] && !(base.vram[at] & 0x8000u))
            break;
    }
    if (i == JBC_VRAM_PIXELS) {
        fail("%s: no pixel matches the dump", l.name);
        return;
    }
    x = (int)(at % JBC_VRAM_W);
    y = (int)(at / JBC_VRAM_W);

    /* One bit of the dump. */
    hw2 = malloc(JBC_VRAM_PIXELS * sizeof *hw2);
    if (hw2 == NULL)
        exit(2);
    memcpy(hw2, hw, JBC_VRAM_PIXELS * sizeof *hw2);
    hw2[at] ^= 1;
    jbc_compare(base.vram, hw2, &d2);
    if (d2.count != d.count + 1 || x < d2.x0 || x > d2.x1 || y < d2.y0 || y > d2.y1)
        fail("%s: a flipped dump pixel gave %lu differences", l.name, d2.count);
    else
        printf("dump pixel (%d, %d) flipped: %lu differ\n", x, y, d2.count);
    free(hw2);

    /* One pixel written by the list. */
    words[0] = JBC_OP(JBC_GP0, 4);
    words[1] = 0xa0000000u;
    words[2] = (uint32_t)x | (uint32_t)y << 16;
    words[3] = 0x00010001u;
    words[4] = (uint32_t)(hw[at] ^ 0x1fu);
    if (jbc_append(&l, words, 5, &l2) != 0)
        exit(2);
    play(&l2, 1, JBC_SPLIT_OP, 0, &changed);
    jbc_compare(changed.vram, hw, &d2);
    if (d2.count != d.count + 1 || x < d2.x0 || x > d2.x1 || y < d2.y0 || y > d2.y1)
        fail("%s: a pixel added to the list gave %lu differences", l.name, d2.count);
    else
        printf("list writes pixel (%d, %d): %lu differ\n", x, y, d2.count);

    jbc_outcome_free(&changed);
    jbc_outcome_free(&base);
    jbc_free(&l2);
    jbc_free(&l);
    free(hw);
}

static void detect_check(const char *list_path)
{
    jbc_list l;
    jbc_outcome base, changed;
    size_t i;
    unsigned seen = 0;

    load(list_path, &l);
    play(&l, 1, JBC_SPLIT_OP, 0, &base);
    printf("%s: %u checks, %u fail\n", l.name, base.checks, base.checks_failed);
    for (i = l.first; JBC_OP_CODE(l.w[i]) != JBC_END; i += 1 + JBC_OP_COUNT(l.w[i])) {
        uint32_t *p = l.w + i + 1, mask;
        if (JBC_OP_CODE(l.w[i]) != JBC_EXPECT)
            continue;
        ++seen;
        if ((base.reads[p[0]] & p[1]) != p[2] || p[1] == 0)
            continue;
        mask = p[1] & (0u - p[1]); /* the mask's lowest bit */
        p[2] ^= mask;
        play(&l, 1, JBC_SPLIT_OP, 0, &changed);
        p[2] ^= mask;
        if (changed.checks_failed != base.checks_failed + 1)
            fail("%s: a changed check gave %u failures", l.name, changed.checks_failed);
        else
            printf("check %u changed: %u fail\n", seen, changed.checks_failed);
        jbc_outcome_free(&changed);
        break;
    }
    if (JBC_OP_CODE(l.w[i]) == JBC_END)
        fail("%s: no passing check to change", l.name);
    jbc_outcome_free(&base);
    jbc_free(&l);
}

static int same(const jbc_outcome *a, const jbc_outcome *b)
{
    return memcmp(a->vram, b->vram, JBC_VRAM_PIXELS * sizeof *a->vram) == 0 &&
           a->nreads == b->nreads &&
           (a->nreads == 0 || memcmp(a->reads, b->reads, a->nreads * sizeof *a->reads) == 0) &&
           a->loops == b->loops && a->passes == b->passes && a->steady == b->steady &&
           a->checks == b->checks &&
           a->checks_failed == b->checks_failed && a->waits == b->waits &&
           a->waits_failed == b->waits_failed && a->state_steady == b->state_steady &&
           a->state_changed_at == b->state_changed_at && a->window_played == b->window_played;
}

static void invariant(const char *list_path)
{
    static const struct {
        int threads;
        jbc_split split;
        uint32_t seed;
        const char *what;
    } runs[] = {
        {1, JBC_SPLIT_WORDS, 0, "a word per call"},
        {1, JBC_SPLIT_RANDOM, 1, "random chunks, seed 1"},
        {1, JBC_SPLIT_RANDOM, 2, "random chunks, seed 2"},
        {1, JBC_SPLIT_RANDOM, 3, "random chunks, seed 3"},
        {4, JBC_SPLIT_OP, 0, "4 threads"},
        {4, JBC_SPLIT_RANDOM, 4, "4 threads, random chunks, seed 4"},
    };
    jbc_list l;
    jbc_outcome ref, other;
    size_t k;

    load(list_path, &l);
    play(&l, 1, JBC_SPLIT_OP, 0, &ref);
    for (k = 0; k < sizeof runs / sizeof runs[0]; ++k) {
        play(&l, runs[k].threads, runs[k].split, runs[k].seed, &other);
        if (!same(&ref, &other))
            fail("%s: differs when played with %s", l.name, runs[k].what);
        jbc_outcome_free(&other);
    }
    printf("%s: the same in %u ways of playing it\n", l.name,
           (unsigned)(sizeof runs / sizeof runs[0] + 1));
    jbc_outcome_free(&ref);
    jbc_free(&l);
}

static void refuse_other_fixture(const char *path, const jbc_fixture *f)
{
    jbc_list l, cut;
    size_t i;
    char err[512];

    load(path, &l);
    if (jbc_check_fixture(&l, f, err, sizeof err) != 0)
        fail("%s is not its own fixture: %s", path, err);
    for (i = l.first; JBC_OP_CODE(l.w[i]) != JBC_EXPECT && JBC_OP_CODE(l.w[i]) != JBC_END;
         i += 1 + JBC_OP_COUNT(l.w[i]))
        ;
    if (JBC_OP_CODE(l.w[i]) != JBC_EXPECT) {
        fail("%s: no EXPECT to cut after", path);
        jbc_free(&l);
        return;
    }
    i += 1 + JBC_OP_COUNT(l.w[i]);
    l.w[i] = (uint32_t)JBC_OP(JBC_END, 0);
    if (jbc_parse(l.w, i + 1, &cut, err, sizeof err) != 0) {
        fail("%s cut after its first EXPECT: %s", path, err);
        jbc_free(&l);
        return;
    }
    if (jbc_check_fixture(&cut, f, err, sizeof err) == 0)
        fail("%s cut after its first EXPECT is taken as the fixture %s", path, f->name);
    else
        printf("%s cut after its first EXPECT (%u checks, %lu words): refused: %s\n", f->name,
               cut.expects, (unsigned long)cut.n, err);
    jbc_free(&cut);
    jbc_free(&l);
}

static void only_documented_exclusion(const char *path, unsigned pixels, uint64_t sum)
{
    unsigned char *ex = calloc(JBC_VRAM_PIXELS, 1);
    unsigned n = 0, entries = 0;
    char err[256];

    if (ex == NULL)
        exit(2);
    if (jbc_load_exclusions(path, ex, &n, &entries, err, sizeof err) != 0)
        fail("%s: %s", path, err);
    else if (n != pixels || jbc_exclusion_checksum(ex) != sum)
        fail("%s: %u pixels, checksum %016llx: not the documented %u, %016llx", path, n,
             (unsigned long long)jbc_exclusion_checksum(ex), pixels, (unsigned long long)sum);
    else
        printf("%s: the documented %u pixels\n", path, n);
    memset(ex, 0, JBC_VRAM_PIXELS);
    if (jbc_parse_exclusions("rect 0 0 1024 512 everything\n", "attack.exclude", ex, &n, &entries,
                             err, sizeof err) != 0)
        fail("attack.exclude: %s", err);
    else if (n == pixels && jbc_exclusion_checksum(ex) == sum)
        fail("an exclusion of all of VRAM passes as the documented one");
    else
        printf("an exclusion of all of VRAM (%u pixels, checksum %016llx): refused\n", n,
               (unsigned long long)jbc_exclusion_checksum(ex));
    free(ex);
}

static size_t put_string(uint32_t *w, const char *str)
{
    size_t len = strlen(str), i, n = 1 + (len + 3) / 4;

    memset(w, 0, n * sizeof *w);
    w[0] = (uint32_t)len;
    for (i = 0; i < len; ++i)
        w[1 + i / 4] |= (uint32_t)(unsigned char)str[i] << (8 * (i % 4));
    return n;
}

/* A list's fixed header and its two strings into w: the words used. */
static size_t list_head(uint32_t *w, const char *name)
{
    size_t n;

    memcpy(w, JBC_MAGIC, 8);
    n = 2;
    w[n++] = JBC_VERSION;
    w[n++] = 0;
    n += put_string(w + n, name);
    n += put_string(w + n, "selftest.c");
    w[3] = (uint32_t)(n * 4);
    return n;
}

/* Plays l to pass `passes` with the window; exits on an error. */
static void play_to(const jbc_list *l, unsigned passes, jbc_outcome *res)
{
    jbc_options o;
    char err[256];

    memset(&o, 0, sizeof o);
    o.passes = passes;
    if (jbc_play(l, &o, res, err, sizeof err) != 0) {
        printf("ERROR %s: %s\n", l->name, err);
        exit(2);
    }
}

/* VRAM never changes, the state does, one step a pass: pass 2
   opens GP1(09h)'s gate, which cannot be read back, and pass 3's
   GP0(E1h) then stores its bit 11 as GPUSTAT bit 15. */
static void gate_chain(void)
{
    uint32_t w[64];
    size_t n = list_head(w, "gpu/gate-chain");
    jbc_list l;
    jbc_outcome res;
    jbc_verdict v;
    char err[256], line[512];
    int status;

    w[n++] = (uint32_t)JBC_OP(JBC_GP1, 1);
    w[n++] = 0x09000000u;
    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 1);
    w[n++] = 0xe1000800u;
    w[n++] = (uint32_t)JBC_OP(JBC_VSYNC, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_LOOP, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 1);
    w[n++] = 0xe1000800u;
    w[n++] = (uint32_t)JBC_OP(JBC_GP1, 1);
    w[n++] = 0x09000001u;
    w[n++] = (uint32_t)JBC_OP(JBC_VSYNC, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_END, 0);
    if (jbc_parse(w, n, &l, err, sizeof err) != 0) {
        fail("gate-chain list: %s", err);
        return;
    }
    play_to(&l, 1, &res);
    memset(&v, 0, sizeof v);
    v.loop_failed = !res.steady || res.early || !res.state_steady;
    status = jbc_judge("gate-chain", &v, line, sizeof line);
    if (!res.steady || res.state_steady || res.state_changed_at != 3 ||
        strstr(res.state_diff, "GPUSTAT") == NULL || status != 1)
        fail("gate-chain, compared at pass 1: VRAM steady %d, state steady %d, changed at pass %u "
             "(%s), status %d",
             res.steady, res.state_steady, res.state_changed_at, res.state_diff, status);
    else
        printf("gate-chain, compared at pass 1: VRAM unchanged, pass %u changed the state (%s): %s\n",
               res.state_changed_at, res.state_diff, line);
    jbc_outcome_free(&res);
    jbc_free(&l);
}

/* A CPU-to-VRAM transfer of 8
   pixels open across LOOP, one payload word a pass. */
static void late_vram_refused(void)
{
    uint32_t w[64];
    size_t n = list_head(w, "gpu/late-vram");
    static const uint32_t first[] = {0xa0000000u, 4, 0x00010001u, 0x7fffu,
                                     0xa0000000u, 0, 0x00010008u};
    jbc_list l;
    char err[256];
    size_t k;

    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 7);
    for (k = 0; k < 7; ++k)
        w[n++] = first[k];
    w[n++] = (uint32_t)JBC_OP(JBC_LOOP, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 1);
    w[n++] = 0;
    w[n++] = (uint32_t)JBC_OP(JBC_END, 0);
    if (jbc_parse(w, n, &l, err, sizeof err) == 0) {
        fail("late-vram: a transfer open across LOOP is taken");
        jbc_free(&l);
    } else if (strstr(err, "LOOP inside a GPU packet") == NULL)
        fail("late-vram refused, but not by the packet rule: %s", err);
    else
        printf("late-vram: refused: %s\n", err);
}

/* A rectangle of raw 4-bit texture from (0, 0) of the page GP0(E1h) set,
   w x 1 at (x, y), with the CLUT at clut (GP0(65h)). */
static size_t rect4(uint32_t *w, int x, int y, int width, uint32_t clut)
{
    w[0] = (uint32_t)JBC_OP(JBC_GP0, 4);
    w[1] = 0x65000000u;
    w[2] = (uint32_t)x | (uint32_t)y << 16;
    w[3] = clut << 16;
    w[4] = (uint32_t)width | 1u << 16;
    return 5;
}

/* The limit FORMAT.md states: a palette carried from pass to pass in the
   CLUT cache alone. The CLUT is A (0, 100), another B (16, 100); the
   texture at (512, 0) has texel x = (x + 1) & 15. Each pass draws the
   texture with the cached palette into R (0, 200), copies R over A, loads
   B and then A again (the cache now holds the palette rotated by one),
   puts A, R and the scratch row S (32, 400) back as they were, and draws
   texel 0 at V (0, 300) with the cache. V after pass k is entry k of the
   first palette: red for k 1 to 4, green from 5 on. */
static void clut_chain(void)
{
    enum { CLUT_A = 100u << 6, CLUT_B = 1u | 100u << 6 };
    uint32_t w[160];
    size_t n = list_head(w, "gpu/clut-chain"), k, at;
    uint32_t pal[8];
    jbc_list l;
    jbc_outcome res;
    char err[256];
    uint16_t v1, v5;

    /* The first palette: entry 0 blue, 1-4 red, 5-15 green. */
    for (k = 0; k < 8; ++k) {
        uint32_t e0 = k == 0 ? 0x7c00u : 2 * k <= 4 ? 0x001fu : 0x03e0u;
        uint32_t e1 = 2 * k + 1 <= 4 ? 0x001fu : 0x03e0u;
        pal[k] = e0 | e1 << 16;
    }
    at = n;
    w[n++] = 0;
    w[n++] = 0xe3000000u;
    w[n++] = 0xe4000000u | 1023u | 511u << 10;
    w[n++] = 0xe5000000u;
    w[n++] = 0xe1000008u; /* page (512, 0), 4-bit */
    w[n++] = 0xa0000000u; /* the texture */
    w[n++] = 512u;
    w[n++] = 0x00010004u;
    w[n++] = 0x87654321u;
    w[n++] = 0x0fedcba9u;
    w[n++] = 0xa0000000u; /* the palette at A */
    w[n++] = 100u << 16;
    w[n++] = 0x00010010u;
    for (k = 0; k < 8; ++k)
        w[n++] = pal[k];
    w[at] = (uint32_t)JBC_OP(JBC_GP0, n - at - 1);
    n += rect4(w + n, 0, 300, 1, CLUT_A); /* V, loading A */
    w[n++] = (uint32_t)JBC_OP(JBC_VSYNC, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_LOOP, 0);
    n += rect4(w + n, 0, 200, 16, CLUT_A); /* R with the cached palette */
    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 4);  /* R over A */
    w[n++] = 0x80000000u;
    w[n++] = 200u << 16;
    w[n++] = 100u << 16;
    w[n++] = 0x00010010u;
    n += rect4(w + n, 32, 400, 16, CLUT_B); /* load B */
    n += rect4(w + n, 32, 400, 16, CLUT_A); /* load A: the rotated palette */
    at = n;
    w[n++] = 0;
    w[n++] = 0xa0000000u; /* A, R and S back */
    w[n++] = 100u << 16;
    w[n++] = 0x00010010u;
    for (k = 0; k < 8; ++k)
        w[n++] = pal[k];
    w[n++] = 0x02000000u;
    w[n++] = 200u << 16;
    w[n++] = 0x00010010u;
    w[n++] = 0x02000000u;
    w[n++] = 32u | 400u << 16;
    w[n++] = 0x00010010u;
    w[at] = (uint32_t)JBC_OP(JBC_GP0, n - at - 1);
    n += rect4(w + n, 0, 300, 1, CLUT_A); /* V with the cache */
    w[n++] = (uint32_t)JBC_OP(JBC_VSYNC, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_END, 0);
    if (jbc_parse(w, n, &l, err, sizeof err) != 0) {
        fail("clut-chain list: %s", err);
        return;
    }
    play_to(&l, 1, &res);
    v1 = res.vram[300 * JBC_VRAM_W];
    if (!res.steady || !res.state_steady || v1 != 0x001fu)
        fail("clut-chain, compared at pass 1: VRAM steady %d, state steady %d (%s), V 0x%04x: the "
             "limit FORMAT.md states is not what the window does",
             res.steady, res.state_steady, res.state_diff, v1);
    jbc_outcome_free(&res);
    play_to(&l, 5, &res);
    v5 = res.vram[300 * JBC_VRAM_W];
    if (!res.steady || res.early || !res.state_steady || v5 != 0x03e0u)
        fail("clut-chain, compared at pass 5: steady %d, early %d, state steady %d, V 0x%04x", res.steady,
             res.early, res.state_steady, v5);
    else
        printf("clut-chain: the window at pass 1 accepts V 0x%04x; pass 5 changes it to 0x%04x (the "
               "CLUT cache is not compared: FORMAT.md)\n",
               v1, v5);
    jbc_outcome_free(&res);
    jbc_free(&l);
}

static void late_loop_change(void)
{
    uint32_t w[64];
    size_t n = 0;
    jbc_list l;
    jbc_options o;
    jbc_outcome res;
    jbc_verdict v;
    char err[256], line[256];
    uint16_t *black = calloc(JBC_VRAM_PIXELS, sizeof *black);
    jbc_diff d;
    int status;

    if (black == NULL)
        exit(2);
    memcpy(w, JBC_MAGIC, 8);
    n = 2;
    w[n++] = JBC_VERSION;
    w[n++] = 0;
    n += put_string(w + n, "gpu/late-loop");
    n += put_string(w + n, "selftest.c");
    w[3] = (uint32_t)(n * 4);
    /* Pass 1: an empty drawing area, (10, 10) to (9, 9). */
    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 2);
    w[n++] = 0xe3000000u | 10u | 10u << 10;
    w[n++] = 0xe4000000u | 9u | 9u << 10;
    w[n++] = (uint32_t)JBC_OP(JBC_VSYNC, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_LOOP, 0);
    /* The loop: a white 16 x 16 tile at (0, 0), then the drawing area made
       all of VRAM. The tile is clipped away in pass 2 and drawn in 3. */
    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 4);
    w[n++] = 0x60ffffffu;
    w[n++] = 0x00000000u;
    w[n++] = 0x00100010u;
    w[n++] = 0xe4000000u | 1023u | 511u << 10;
    w[n++] = (uint32_t)JBC_OP(JBC_VSYNC, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_END, 0);
    if (jbc_parse(w, n, &l, err, sizeof err) != 0) {
        fail("late-loop list: %s", err);
        free(black);
        return;
    }

    memset(&o, 0, sizeof o);
    play(&l, 1, JBC_SPLIT_OP, 0, &res); /* the old way */
    jbc_compare(res.vram, black, &d);
    printf("late-loop, played until a pass changes nothing: stops after pass %u, %lu pixels "
           "differ from a black dump\n",
           res.passes, d.count);
    jbc_outcome_free(&res);

    o.passes = 1;
    if (jbc_play(&l, &o, &res, err, sizeof err) != 0)
        exit(2);
    jbc_compare(res.vram, black, &d);
    memset(&v, 0, sizeof v);
    v.vram_differs = d.count != 0;
    v.loop_failed = !res.steady || res.early;
    v.loop_failed = !res.steady || res.early || !res.state_steady;
    status = jbc_judge("late-loop", &v, line, sizeof line);
    if (res.steady || res.changed_at != 3 || res.state_steady || res.state_changed_at != 2 ||
        strstr(res.state_diff, "bottom right") == NULL || status == 0)
        fail("late-loop, compared at pass 1: steady %d, changed at pass %u, state steady %d, changed "
             "at pass %u (%s), status %d",
             res.steady, res.changed_at, res.state_steady, res.state_changed_at, res.state_diff,
             status);
    else
        printf("late-loop, compared at pass 1: pass %u changed the state (%s), pass %u VRAM: %s\n",
               res.state_changed_at, res.state_diff, res.changed_at, line);
    jbc_outcome_free(&res);

    /* The same list stated at pass 2 is refused as well: pass 2 changed
       nothing, so the fixed point is not where it says. */
    o.passes = 2;
    if (jbc_play(&l, &o, &res, err, sizeof err) != 0)
        exit(2);
    if (!res.early && res.steady)
        fail("late-loop, compared at pass 2: accepted");
    jbc_outcome_free(&res);
    free(black);
    jbc_free(&l);
}

static void verdicts(void)
{
    static const struct {
        jbc_verdict v;
        int status;
        const char *starts;
    } cases[] = {
        /* vram, checks, waits, loop, expect-mismatch, excluded, reads, console checks */
        {{0, 0, 0, 0, 0, 0, 0, 0}, 0, "x: MATCHES"},
        {{0, 0, 0, 0, 0, 64, 0, 0}, 0, "x: MATCHES (64 pixels excluded)"},
        {{1, 0, 0, 0, 0, 0, 0, 0}, 1, "x: DIFFERS"},
        {{0, 1, 0, 0, 0, 0, 0, 0}, 1, "x: DIFFERS"},
        {{0, 0, 1, 0, 0, 0, 0, 0}, 1, "x: DIFFERS: a wait"},
        {{1, 0, 0, 0, 1, 0, 0, 0}, 0, "x: DIFFERS, as expected"},
        {{0, 2, 0, 0, 1, 0, 0, 0}, 0, "x: DIFFERS, as expected"},
        {{0, 0, 0, 0, 1, 0, 0, 0}, 1, "x: MATCHES, but is listed"},
        /* A failed wait in an otherwise matching list */
        {{0, 0, 1, 0, 1, 0, 0, 0}, 1, "x: DIFFERS: a wait"},
        {{1, 0, 1, 0, 1, 0, 0, 0}, 1, "x: DIFFERS: a wait"},
        {{0, 0, 0, 1, 1, 0, 0, 0}, 1, "x: DIFFERS: the main loop"},
        /* A probe result's reads (conformance.c --probe) */
        {{0, 0, 0, 0, 0, 0, 3, 0}, 1, "x: DIFFERS"},
        {{0, 0, 0, 0, 1, 0, 3, 0}, 0, "x: DIFFERS, as expected"},
        /* A check the probe's console failed fails, whatever else */
        {{0, 0, 0, 0, 0, 0, 0, 1}, 1, "x: MATCHES the console's result, but the console failed 1"},
        {{0, 0, 0, 0, 1, 0, 0, 2}, 1, "x: MATCHES the console's result, but the console failed 2"},
        {{1, 0, 0, 0, 0, 0, 0, 1}, 1, "x: DIFFERS from the console's result, but the console"},
    };
    size_t k;
    char line[512];

    for (k = 0; k < sizeof cases / sizeof cases[0]; ++k) {
        int status = jbc_judge("x", &cases[k].v, line, sizeof line);
        if (status != cases[k].status ||
            strncmp(line, cases[k].starts, strlen(cases[k].starts)) != 0)
            fail("verdict %u: %d \"%s\", expected %d \"%s...\"", (unsigned)k, status, line,
                 cases[k].status, cases[k].starts);
    }
    printf("verdicts: %u cases\n", (unsigned)(sizeof cases / sizeof cases[0]));
}

/* 8: writes text to DIR/name and looks key up in it: 0 when the table is
   taken, else -1 with the reason in err. */
static int lookup_in(const char *dir, const char *name, const char *text, const char *key,
                     char *err, size_t size)
{
    char path[1024];
    jbc_table_entry e;
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "w");
    if (f == NULL || fputs(text, f) == EOF || fclose(f) != 0) {
        printf("ERROR cannot write %s\n", path);
        exit(2);
    }
    return jbc_table_lookup(path, key, &e, err, size);
}

static void tables(const char *table, const char *dir)
{
    static const struct {
        const char *name, *extra, *why;
    } bad[] = {
        {"other-malformed.txt", "other repo dump 1 0 2756 db852b116de72493 efdc1651b890309e x.exclude 64\n",
         "malformed"},
        {"other-no-refsum.txt", "other repo dump 1 0 2756 db852b116de72493\n", "malformed"},
        {"other-probe-refsum.txt", "other open probe 0 0 94 66155f7a6c482012 efdc1651b890309e\n",
         "malformed"},
        {"other-log-exclude.txt",
         "other repo log 1 5 493 67a419839f6e98b8 ccfa6ba800604ade x.exclude 64 8dac3f054012b465\n",
         "malformed"},
    };
    FILE *f = fopen(table, "r");
    char *text, *with, quad[512] = "", line[512], err[512];
    size_t len = 0, cap = 1 << 16, k;

    text = malloc(cap);
    with = malloc(cap + 1024);
    if (f == NULL || text == NULL || with == NULL) {
        printf("ERROR cannot read %s\n", table);
        exit(2);
    }
    text[0] = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strlen(line);
        if (len + n + 1 > cap) {
            printf("ERROR %s is too long for the self-test\n", table);
            exit(2);
        }
        memcpy(text + len, line, n + 1);
        len += n;
        if (strncmp(line, "quad ", 5) == 0)
            snprintf(quad, sizeof quad, "%s", line);
    }
    fclose(f);
    if (quad[0] == 0) {
        fail("%s has no line for quad", table);
        free(text);
        free(with);
        return;
    }
    if (lookup_in(dir, "as-is.txt", text, "triangle", err, sizeof err) != 0)
        fail("the table as it is: %s", err);
    snprintf(with, cap + 1024, "%s%s", text, quad);
    if (lookup_in(dir, "other-twice.txt", with, "triangle", err, sizeof err) == 0)
        fail("a table with a second line for quad is taken for triangle");
    else if (strstr(err, "a second line for quad") == NULL)
        fail("a table with a second line for quad, refused for another reason: %s", err);
    else
        printf("a second line for quad, triangle asked for: refused: %s\n", err);
    {
        /* A zero byte before a second line for quad, and no final newline:
           read as C strings, the second line would hide behind the zero. */
        char path[1024];
        jbc_table_entry e;
        size_t tl = strlen(text), ql = strlen(quad);
        FILE *z;

        while (ql > 0 && (quad[ql - 1] == '\n' || quad[ql - 1] == '\r'))
            --ql;
        snprintf(path, sizeof path, "%s/%s", dir, "zero-byte.txt");
        z = fopen(path, "wb");
        if (z == NULL || fwrite(text, 1, tl, z) != tl || fputc(0, z) == EOF ||
            fwrite(quad, 1, ql, z) != ql || fclose(z) != 0) {
            printf("ERROR cannot write %s\n", path);
            exit(2);
        }
        if (jbc_table_lookup(path, "triangle", &e, err, sizeof err) == 0)
            fail("a table with a zero byte hiding a second line for quad is taken for triangle");
        else if (strstr(err, "zero byte") == NULL)
            fail("a table with a zero byte, refused for another reason: %s", err);
        else
            printf("a zero byte in the table: refused: %s\n", err);
    }
    for (k = 0; k < sizeof bad / sizeof bad[0]; ++k) {
        snprintf(with, cap + 1024, "%s%s", text, bad[k].extra);
        if (lookup_in(dir, bad[k].name, with, "triangle", err, sizeof err) == 0)
            fail("%s is taken for triangle", bad[k].name);
        else if (strstr(err, bad[k].why) == NULL)
            fail("%s, refused for another reason: %s", bad[k].name, err);
        else
            printf("%s, triangle asked for: refused: %s\n", bad[k].name, err);
    }
    free(text);
    free(with);
}

/* 9: a list of the ops given (code, then its payload) and END. */
static int packet_list(const char *name, const uint32_t *ops, size_t n, char *err, size_t size)
{
    uint32_t w[64];
    size_t at = list_head(w, name);
    jbc_list l;

    memcpy(w + at, ops, n * sizeof *ops);
    at += n;
    w[at++] = (uint32_t)JBC_OP(JBC_END, 0);
    if (jbc_parse(w, at, &l, err, size) != 0)
        return -1;
    jbc_free(&l);
    return 0;
}

static void packets(void)
{
#define UPLOAD (uint32_t)JBC_OP(JBC_GP0, 3), 0xa0000000u, 0, 0x00010003u
    static const uint32_t odd[] = {UPLOAD, (uint32_t)JBC_OP(JBC_GP0, 2), 0, 0};
    static const uint32_t short_odd[] = {UPLOAD, (uint32_t)JBC_OP(JBC_GP0, 1), 0};
    static const uint32_t reset[] = {UPLOAD, (uint32_t)JBC_OP(JBC_GP1, 1), 0x01000000u};
    static const uint32_t mirror41[] = {UPLOAD, (uint32_t)JBC_OP(JBC_GP1, 1), 0x41000000u};
    static const uint32_t mirror81[] = {UPLOAD, (uint32_t)JBC_OP(JBC_GP1, 1), 0x81000000u};
    static const uint32_t ack[] = {UPLOAD, (uint32_t)JBC_OP(JBC_GP1, 1), 0x02000000u};
    static const uint32_t line_flat[] = {(uint32_t)JBC_OP(JBC_GP0, 4), 0x48000000u, 0, 0,
                                         0x50005000u};
    static const uint32_t line_shaded[] = {(uint32_t)JBC_OP(JBC_GP0, 5), 0x58000000u, 0, 0, 0,
                                           0x50005000u};
    static const uint32_t line_false_end[] = {(uint32_t)JBC_OP(JBC_GP0, 6), 0x58000000u, 0, 0, 0,
                                              0, 0x50005000u};
    static const uint32_t irq[] = {(uint32_t)JBC_OP(JBC_GP0, 1), 0x1f000000u};
    static const uint32_t unknown[] = {(uint32_t)JBC_OP(JBC_GP0, 1), 0x03000000u};
#undef UPLOAD
    static const struct {
        const char *name;
        const uint32_t *ops;
        size_t n;
        int between;
    } cases[] = {
        {"gpu/odd", odd, sizeof odd / 4, 1},
        {"gpu/short-odd", short_odd, sizeof short_odd / 4, 0},
        {"gpu/reset", reset, sizeof reset / 4, 1},
        {"gpu/reset-mirror", mirror41, sizeof mirror41 / 4, 1},
        {"gpu/reset-mirror-81", mirror81, sizeof mirror81 / 4, 1},
        {"gpu/ack-irq", ack, sizeof ack / 4, 0},
        {"gpu/line-flat", line_flat, sizeof line_flat / 4, 1},
        {"gpu/line-shaded", line_shaded, sizeof line_shaded / 4, 1},
        {"gpu/line-false-end", line_false_end, sizeof line_false_end / 4, 0},
        {"gpu/irq", irq, sizeof irq / 4, 1},
        {"gpu/unknown", unknown, sizeof unknown / 4, 1},
    };
    size_t k;
    char err[256];

    for (k = 0; k < sizeof cases / sizeof cases[0]; ++k) {
        int ok = packet_list(cases[k].name, cases[k].ops, cases[k].n, err, sizeof err) == 0;
        if (ok != cases[k].between)
            fail("%s: %s", cases[k].name,
                 ok ? "taken, though END is inside a packet" : err);
        else if (!ok && strstr(err, "END inside a GPU packet") == NULL)
            fail("%s refused, but not by the packet rule: %s", cases[k].name, err);
    }
    printf("packets: %u lists, each between packets at END or not as psx-spx counts them\n",
           (unsigned)(sizeof cases / sizeof cases[0]));
}

/* 10: pass 1 fills a row red, the loop fills it green, so pass 2 changes
   VRAM: compared at pass 1, the window stops at pass 2. */
static void window_report(void)
{
    uint32_t w[64];
    size_t n = list_head(w, "gpu/one-pass");
    jbc_list l;
    jbc_outcome res;
    char err[256], text[1024];

    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 3);
    w[n++] = 0x020000ffu;
    w[n++] = 0;
    w[n++] = 0x00010010u;
    w[n++] = (uint32_t)JBC_OP(JBC_LOOP, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 3);
    w[n++] = 0x0200ff00u;
    w[n++] = 0;
    w[n++] = 0x00010010u;
    w[n++] = (uint32_t)JBC_OP(JBC_END, 0);
    if (jbc_parse(w, n, &l, err, sizeof err) != 0) {
        fail("one-pass list: %s", err);
        return;
    }
    play_to(&l, 1, &res);
    jbc_describe_loop(&res, text, sizeof text);
    if (res.steady || res.changed_at != 2 || res.window_played != 1 || res.window != 2 ||
        !res.state_steady || strstr(text, "pass 2 changed VRAM") == NULL ||
        strstr(text, "pass 2 left GPUSTAT") == NULL || strstr(text, "not played") == NULL ||
        strstr(text, "3") != NULL)
        fail("one-pass, compared at pass 1: steady %d, changed at %u, %u of %u window passes; "
             "reported:\n%s",
             res.steady, res.changed_at, res.window_played, res.window, text);
    else
        printf("one-pass, compared at pass 1, reported:\n%s\n", text);
    jbc_outcome_free(&res);
    jbc_free(&l);
}

static int usage(void)
{
    printf("usage: selftest --dump-list LIST DUMP --check-list LIST --fixtures TABLE --cut NAME LIST "
           "--exclusion-of NAME --scratch DIR --lists LIST...\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *dump_list = NULL, *dump = NULL, *check_list = NULL, *cut_list = NULL;
    const char *table = NULL, *cut_name = NULL, *ex_name = NULL, *scratch = NULL;
    jbc_table_entry cut, ex;
    char err[512], path[1024];
    const char *slash;
    int a = 1, lists = 0;

    while (a < argc) {
        if (strcmp(argv[a], "--dump-list") == 0 && a + 2 < argc) {
            dump_list = argv[a + 1];
            dump = argv[a + 2];
            a += 3;
        } else if (strcmp(argv[a], "--check-list") == 0 && a + 1 < argc) {
            check_list = argv[a + 1];
            a += 2;
        } else if (strcmp(argv[a], "--fixtures") == 0 && a + 1 < argc) {
            table = argv[a + 1];
            a += 2;
        } else if (strcmp(argv[a], "--cut") == 0 && a + 2 < argc) {
            cut_name = argv[a + 1];
            cut_list = argv[a + 2];
            a += 3;
        } else if (strcmp(argv[a], "--exclusion-of") == 0 && a + 1 < argc) {
            ex_name = argv[a + 1];
            a += 2;
        } else if (strcmp(argv[a], "--scratch") == 0 && a + 1 < argc) {
            scratch = argv[a + 1];
            a += 2;
        } else if (strcmp(argv[a], "--lists") == 0) {
            lists = a + 1;
            break;
        } else
            return usage();
    }
    if (dump_list == NULL || check_list == NULL || table == NULL || cut_name == NULL ||
        ex_name == NULL || scratch == NULL || lists == 0)
        return usage();
    if (jbc_table_lookup(table, cut_name, &cut, err, sizeof err) != 0 ||
        jbc_table_lookup(table, ex_name, &ex, err, sizeof err) != 0) {
        printf("ERROR %s\n", err);
        return 2;
    }
    if (ex.exclude[0] == 0) {
        printf("ERROR %s names no exclusion for %s\n", table, ex_name);
        return 2;
    }
    slash = strrchr(table, '/');
    snprintf(path, sizeof path, "%.*s/%.*s", slash ? (int)(slash - table) : 1, slash ? table : ".",
             (int)(sizeof path / 2), ex.exclude);
    detect_pixel(dump_list, dump);
    detect_check(check_list);
    refuse_other_fixture(cut_list, &cut.fixture);
    only_documented_exclusion(path, ex.exclude_pixels, ex.exclude_checksum);
    late_loop_change();
    gate_chain();
    late_vram_refused();
    clut_chain();
    verdicts();
    tables(table, scratch);
    packets();
    window_report();
    for (a = lists; a < argc; ++a)
        invariant(argv[a]);
    printf("selftest: %d failures\n", failures);
    return failures != 0;
}

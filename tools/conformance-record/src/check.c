/*
 * Checks: test.h's assertions as EXPECT ops (docs/testing.md, "How
 * `tools/conformance-record` works",
 * "Main loop & checks").
 *
 * An assertion compares a value the program computed from one read of
 * GPUREAD or GPUSTAT with a constant. The console's log (JBREC_PSXLOG,
 * ps1-tests' psx.log) says what the console gave: "pass - name" means the
 * value was the constant, and "fail - name:line ..., given: 0xG, ..."
 * means it was G. The check written is "(read & mask) == value", with the
 * read's index in the list, the console's value, and a mask found by
 * evaluating the program's expression again with chosen read values
 * (while nothing is recorded): the expression must be of that form, or
 * the recording stops. Each assertion is matched to the next pass or fail
 * line of the log, by function name; every such line must be matched.
 */
#include "rec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int jbrec_assert(const char *function, int line, const char *given, const char *expected,
                 uint32_t (*eval)(void *), void *closure, uint32_t want);
void jbrec_unsupported(const char *what);

typedef struct {
    char name[96];
    int pass;
    uint32_t given;
} Outcome;

static struct {
    int loaded;
    Outcome *o;
    size_t n, next;
} L;

void jbrec_unsupported(const char *what)
{
    rec_fail("%s is not supported", what);
}

/* The console's outcomes, in order, from the log. */
static void load_log(void)
{
    const char *path = getenv("JBREC_PSXLOG");
    size_t size, cap = 0;
    char *text, *p;

    L.loaded = 1;
    if (path == NULL)
        rec_fail("the program checks results, but JBREC_PSXLOG is not set");
    text = (char *)rec_read_file(path, &size);
    for (p = text; *p;) {
        char *end = strchr(p, '\n');
        Outcome o;
        size_t len;
        if (end != NULL)
            *end = 0;
        memset(&o, 0, sizeof o);
        if (strncmp(p, "pass - ", 7) == 0 || strncmp(p, "fail - ", 7) == 0) {
            const char *name = p + 7;
            o.pass = p[0] == 'p';
            len = o.pass ? strcspn(name, "\r") : strcspn(name, ":");
            if (len == 0 || len >= sizeof o.name)
                rec_fail("%s: bad line: %s", path, p);
            memcpy(o.name, name, len);
            if (!o.pass) {
                const char *g = strstr(name, "given: 0x");
                if (g == NULL)
                    rec_fail("%s: a fail line without its value: %s", path, p);
                o.given = (uint32_t)strtoul(g + 9, NULL, 16);
            }
            if (L.n == cap) {
                cap = cap ? cap * 2 : 16;
                L.o = realloc(L.o, cap * sizeof *L.o);
                if (L.o == NULL)
                    rec_fail("out of memory");
            }
            L.o[L.n++] = o;
        }
        if (end == NULL)
            break;
        p = end + 1;
    }
    free(text);
}

static uint32_t probe(uint32_t (*eval)(void *), void *closure, uint32_t v, const char *where)
{
    uint32_t got;
    unsigned reads;

    rec_probe_begin(v);
    got = eval(closure);
    reads = rec_probe_end();
    if (reads != 1)
        rec_fail("%s: evaluated again, the checked value made %u reads", where, reads);
    return got;
}

int jbrec_assert(const char *function, int line, const char *given, const char *expected,
                 uint32_t (*eval)(void *), void *closure, uint32_t want)
{
    static const uint32_t extra[] = {0x00000000u, 0x12345678u, 0x9abcdef0u, 0x0f0f0f0fu,
                                     0xf0f0f0f0u, 0x5555aaaau, 0x8000ffffu};
    char where[160], label[512];
    uint32_t first, mask, value;
    unsigned i;
    const Outcome *o;

    snprintf(where, sizeof where, "%s:%d", function, line);
    if (rec_probing())
        rec_fail("%s: an assertion inside a checked expression", where);

    /* The program's own evaluation, recorded. */
    first = rec_reads();
    (void)eval(closure);
    if (rec_reads() - first != 1)
        rec_fail("%s: the checked value depends on %u reads; one is supported", where,
                 rec_reads() - first);

    /* Its form: (read & mask). */
    mask = probe(eval, closure, 0xffffffffu, where);
    for (i = 0; i < 32; ++i)
        if (probe(eval, closure, 1u << i, where) != ((1u << i) & mask))
            rec_fail("%s: `%s` is not a read and a mask", where, given);
    for (i = 0; i < sizeof extra / sizeof extra[0]; ++i)
        if (probe(eval, closure, extra[i], where) != (extra[i] & mask))
            rec_fail("%s: `%s` is not a read and a mask", where, given);

    /* What the console gave. */
    if (!L.loaded)
        load_log();
    if (L.next == L.n)
        rec_fail("%s: no line left in the console's log for it", where);
    o = &L.o[L.next++];
    if (strcmp(o->name, function) != 0)
        rec_fail("%s: the console's log has %s here", where, o->name);
    value = o->pass ? want : o->given;
    if ((value & ~mask) != 0)
        rec_fail("%s: the console's value 0x%x has bits outside the mask 0x%x", where, value,
                 mask);

    snprintf(label, sizeof label, "%s: %s == %s", function, given, expected);
    rec_expect(first, mask, value, label);
    fprintf(stderr, "conformance-record: check %s: read %u & 0x%08x == 0x%x (console: %s)\n",
            function, first, mask, value, o->pass ? "pass" : "fail");
    return 1;
}

void check_finish(void)
{
    if (!L.loaded && getenv("JBREC_PSXLOG") != NULL)
        load_log();
    if (L.loaded && L.next != L.n)
        rec_fail("%zu lines of the console's log were not matched to a check, from %s",
                 L.n - L.next, L.o[L.next].name);
}

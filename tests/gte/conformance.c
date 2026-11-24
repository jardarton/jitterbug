/*
 * gte_conformance: a file of GTE results read from a console
 * (third_party/ps1-tests/gte, test-all.txt or gte-fuzz.txt; the format is
 * in each file's header) replayed through the public API, every register
 * compared exactly.
 *
 *   test_gte_conformance FILE --cases N --per-command M [--case K]
 *
 * --cases and --per-command state what the file must hold: cases numbered
 * 1 to N, each once, M of them for each of the 22 commands and M without a
 * command (22 M + M = N); a file that holds anything else is an error, so
 * that a test cannot pass on part of the suite. --case K replays case K
 * alone, and K must be one of them.
 *
 * Each case, on one context: the prelude (0 written to registers 0-63 in
 * order, when the file says "prelude clear"), the input set's 64 words
 * written with jb_gte_write to registers 0-63 in that order, the command
 * with jb_gte_command (none for "-"), then all 64 registers read with
 * jb_gte_read and compared with the expected words, FLAG included. That is
 * what the upstream programs did with mtc2/ctc2, cop2 and mfc2/cfc2, in
 * the same order.
 *
 * Two passes over all the cases: on a context with the default options
 * (the console), and on one with precise_vertices and perspective_correct
 * on, with jb_gte_begin_frame before each case, where RTPS and RTPT also
 * record their projections, which must change no register.
 *
 * Every failing case is printed with its pass, number, command, input set
 * and each differing register (expected, got); then, per pass, a table of
 * cases and failures by command. Exit status 0 when every case matches, 1
 * when one does not, 2 when the file is unreadable, malformed or not what
 * --cases and --per-command state, or --case names no case. Strict C99
 * against <jitterbug.h> alone.
 */
#include <jitterbug.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const reg_names[64] = {
    "VXY0", "VZ0", "VXY1", "VZ1", "VXY2", "VZ2", "RGBC", "OTZ",
    "IR0", "IR1", "IR2", "IR3", "SXY0", "SXY1", "SXY2", "SXYP",
    "SZ0", "SZ1", "SZ2", "SZ3", "RGB0", "RGB1", "RGB2", "RES1",
    "MAC0", "MAC1", "MAC2", "MAC3", "IRGB", "ORGB", "LZCS", "LZCR",
    "RT11RT12", "RT13RT21", "RT22RT23", "RT31RT32", "RT33", "TRX", "TRY", "TRZ",
    "L11L12", "L13L21", "L22L23", "L31L32", "L33", "RBK", "GBK", "BBK",
    "LR1LR2", "LR3LG1", "LG2LG3", "LB1LB2", "LB3", "RFC", "GFC", "BFC",
    "OFX", "OFY", "H", "DQA", "DQB", "ZSF3", "ZSF4", "FLAG",
};

/* Command numbers (bits 0-5), and NO_COMMAND. */
#define NO_COMMAND 64
static const char *command_name(int op)
{
    switch (op) {
    case 0x01: return "RTPS";
    case 0x06: return "NCLIP";
    case 0x0c: return "OP";
    case 0x10: return "DPCS";
    case 0x11: return "INTPL";
    case 0x12: return "MVMVA";
    case 0x13: return "NCDS";
    case 0x14: return "CDP";
    case 0x16: return "NCDT";
    case 0x1b: return "NCCS";
    case 0x1c: return "CC";
    case 0x1e: return "NCS";
    case 0x20: return "NCT";
    case 0x28: return "SQR";
    case 0x29: return "DCPL";
    case 0x2a: return "DPCT";
    case 0x2d: return "AVSZ3";
    case 0x2e: return "AVSZ4";
    case 0x30: return "RTPT";
    case 0x3d: return "GPF";
    case 0x3e: return "GPL";
    case 0x3f: return "NCCT";
    case NO_COMMAND: return "none";
    default: return NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* Reading the file. */

struct gte_case {
    unsigned long number, input;
    int has_command;
    uint32_t command;
    uint32_t expected[64];
};

struct suite {
    int clear;
    unsigned long n_inputs, n_cases;
    uint32_t (*inputs)[64];
    struct gte_case *cases;
};

static const char *file_name;
static long line_no;

static void malformed(const char *what)
{
    printf("%s:%ld: %s\n", file_name, line_no, what);
    exit(2);
}

static void *must_calloc(unsigned long n, size_t size)
{
    void *p = calloc(n ? n : 1, size);

    if (p == NULL) {
        printf("out of memory\n");
        exit(2);
    }
    return p;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        printf("%s: cannot read\n", path);
        exit(2);
    }
    buf = must_calloc((unsigned long)n + 1, 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        printf("%s: cannot read\n", path);
        exit(2);
    }
    fclose(f);
    return buf;
}

/* The next space-separated field of *p, or NULL at the end of the line. */
static char *field(char **p)
{
    char *s = *p, *e;

    if (*s == '\0')
        return NULL;
    e = strchr(s, ' ');
    if (e != NULL) {
        *e = '\0';
        *p = e + 1;
    } else {
        *p = s + strlen(s);
    }
    return s;
}

static unsigned long number(const char *s, int base, unsigned long max)
{
    char *end;
    unsigned long v;

    if (s == NULL || *s == '\0' || *s == '-' || *s == '+' || *s == ' ')
        malformed("expected a number");
    v = strtoul(s, &end, base);
    if (*end != '\0' || v > max)
        malformed("bad number");
    return v;
}

static uint32_t word(const char *s)
{
    if (s == NULL || strlen(s) != 8)
        malformed("expected an 8-digit word");
    return (uint32_t)number(s, 16, 0xffffffffUL);
}

static void end_of_line(const char *p)
{
    if (*p != '\0')
        malformed("trailing fields");
}

static void read_case(struct suite *s, unsigned long seen, char *p)
{
    struct gte_case *c = &s->cases[seen];
    char *cmd, *f;
    int last_reg = -1;

    c->number = number(field(&p), 10, 1000000);
    cmd = field(&p);
    if (cmd == NULL)
        malformed("no command");
    c->has_command = strcmp(cmd, "-") != 0;
    if (c->has_command) {
        c->command = word(cmd);
        if (command_name((int)(c->command & 0x3f)) == NULL)
            malformed("not a GTE command");
    }
    c->input = number(field(&p), 10, 1000000);
    if (c->input >= s->n_inputs)
        malformed("no such input set");
    memcpy(c->expected, s->inputs[c->input], sizeof c->expected);
    while ((f = field(&p)) != NULL) {
        char *eq = strchr(f, '=');
        int reg;

        if (eq == NULL)
            malformed("expected reg=word");
        *eq = '\0';
        reg = (int)number(f, 10, 63);
        if (reg <= last_reg)
            malformed("registers not in ascending order");
        last_reg = reg;
        c->expected[reg] = word(eq + 1);
    }
}

static void read_suite(struct suite *s, char *text)
{
    unsigned long inputs_seen = 0, cases_seen = 0;
    char *line, *next;
    int i;

    s->clear = -1;
    for (line = text; *line != '\0'; line = next) {
        char *p = line, *kind;

        line_no++;
        next = strchr(line, '\n');
        if (next == NULL)
            malformed("no newline at the end");
        *next++ = '\0';
        if (line[0] == '#')
            continue;
        kind = field(&p);
        if (kind == NULL) {
            malformed("empty line");
        } else if (strcmp(kind, "source") == 0) {
            continue;
        } else if (strcmp(kind, "prelude") == 0) {
            if (strcmp(p, "clear") == 0)
                s->clear = 1;
            else if (strcmp(p, "none") == 0)
                s->clear = 0;
            else
                malformed("prelude");
        } else if (strcmp(kind, "inputs") == 0 && s->inputs == NULL) {
            s->n_inputs = number(field(&p), 10, 1000000);
            end_of_line(p);
            s->inputs = must_calloc(s->n_inputs, sizeof *s->inputs);
        } else if (strcmp(kind, "cases") == 0 && s->cases == NULL) {
            s->n_cases = number(field(&p), 10, 1000000);
            end_of_line(p);
            s->cases = must_calloc(s->n_cases, sizeof *s->cases);
        } else if (strcmp(kind, "in") == 0) {
            if (s->inputs == NULL || inputs_seen >= s->n_inputs ||
                number(field(&p), 10, 1000000) != inputs_seen)
                malformed("input set out of order, or more than the header says");
            for (i = 0; i < 64; ++i)
                s->inputs[inputs_seen][i] = word(field(&p));
            end_of_line(p);
            inputs_seen++;
        } else if (strcmp(kind, "case") == 0) {
            if (s->clear < 0 || s->cases == NULL || inputs_seen != s->n_inputs ||
                cases_seen >= s->n_cases)
                malformed("case before the header and input sets, or more than it says");
            read_case(s, cases_seen++, p);
        } else {
            malformed("unknown line");
        }
    }
    if (inputs_seen != s->n_inputs || cases_seen != s->n_cases || s->n_cases == 0)
        malformed("fewer input sets or cases than the header says");
}

/* ------------------------------------------------------------------------ */
/* Replay. */

static void must_write(jb_context *jb, int reg, uint32_t v)
{
    if (jb_gte_write(jb, reg, v) != JB_OK) {
        printf("jb_gte_write(%d): %s\n", reg, jb_error_string(jb));
        exit(2);
    }
}

/* Replays the suite (or case `only`, if not negative) on a new context;
   returns the number of failing cases. */
static int replay(const struct suite *s, int options, long only)
{
    const char *pass = options ? "precise" : "console";
    struct {
        int cases, failures;
    } by_op[NO_COMMAND + 1];
    jb_config cfg;
    jb_context *jb;
    jb_result r = JB_ERROR_UNSUPPORTED;
    unsigned long n;
    int failures = 0, i;

    memset(by_op, 0, sizeof by_op);
    memset(&cfg, 0, sizeof cfg);
    cfg.precise_vertices = options;
    cfg.perspective_correct = options;
    jb = jb_create(&cfg, &r);
    if (jb == NULL || r != JB_OK) {
        printf("jb_create: %s\n", jb_result_string(r));
        exit(2);
    }
    for (n = 0; n < s->n_cases; ++n) {
        const struct gte_case *c = &s->cases[n];
        const uint32_t *in = s->inputs[c->input];
        int op = c->has_command ? (int)(c->command & 0x3f) : NO_COMMAND;
        uint32_t got[64];
        int bad = 0;

        if (only >= 0 && c->number != (unsigned long)only)
            continue;
        if (options && jb_gte_begin_frame(jb) != JB_OK)
            exit(2);
        if (s->clear)
            for (i = 0; i < 64; ++i)
                must_write(jb, i, 0);
        for (i = 0; i < 64; ++i)
            must_write(jb, i, in[i]);
        if (c->has_command && jb_gte_command(jb, c->command) != JB_OK) {
            printf("%s: case %lu: jb_gte_command(0x%08lx): %s\n", pass, c->number,
                   (unsigned long)c->command, jb_error_string(jb));
            bad = 1;
        }
        for (i = 0; i < 64; ++i) {
            got[i] = jb_gte_read(jb, i);
            if (got[i] != c->expected[i])
                bad = 1;
        }
        by_op[op].cases++;
        if (!bad)
            continue;
        by_op[op].failures++;
        failures++;
        if (c->has_command) {
            uint32_t w = c->command;

            printf("%s: case %lu: %s 0x%08lx (sf=%d lm=%d mx=%d v=%d cv=%d), input set %lu\n",
                   pass, c->number, command_name(op), (unsigned long)w, (int)(w >> 19 & 1),
                   (int)(w >> 10 & 1), (int)(w >> 17 & 3), (int)(w >> 15 & 3),
                   (int)(w >> 13 & 3), c->input);
        } else {
            printf("%s: case %lu: no command, input set %lu\n", pass, c->number, c->input);
        }
        for (i = 0; i < 64; ++i)
            if (got[i] != c->expected[i])
                printf("  %2d %-8s expected %08lx got %08lx\n", i, reg_names[i],
                       (unsigned long)c->expected[i], (unsigned long)got[i]);
    }
    jb_destroy(jb);

    printf("%s pass:\n  %-6s %6s %8s\n", pass, "cmd", "cases", "failures");
    for (i = 0; i <= NO_COMMAND; ++i)
        if (by_op[i].cases > 0)
            printf("  %-6s %6d %8d\n", command_name(i), by_op[i].cases, by_op[i].failures);
    return failures;
}

/* The file holds exactly what the test states: cases 1..cases, each once,
   per_command for each command and for none. */
static void check_counts(const struct suite *s, unsigned long cases, unsigned long per_command)
{
    unsigned long n, by_op[NO_COMMAND + 1];
    unsigned char *seen;
    int op, commands = 0;

    memset(by_op, 0, sizeof by_op);
    if (s->n_cases != cases) {
        printf("%s: %lu cases, not the %lu stated\n", file_name, s->n_cases, cases);
        exit(2);
    }
    seen = must_calloc(cases + 1, 1);
    for (n = 0; n < s->n_cases; ++n) {
        const struct gte_case *c = &s->cases[n];
        if (c->number < 1 || c->number > cases || seen[c->number]) {
            printf("%s: case number %lu out of 1..%lu or repeated\n", file_name, c->number, cases);
            exit(2);
        }
        seen[c->number] = 1;
        by_op[c->has_command ? (int)(c->command & 0x3f) : NO_COMMAND]++;
    }
    free(seen);
    for (op = 0; op <= NO_COMMAND; ++op) {
        if (command_name(op) == NULL)
            continue;
        commands += op != NO_COMMAND;
        if (by_op[op] != per_command) {
            printf("%s: %lu cases of %s, not the %lu stated\n", file_name, by_op[op],
                   command_name(op), per_command);
            exit(2);
        }
    }
    printf("%s: cases 1..%lu, %lu for each of %d commands and %lu without one\n", file_name,
           cases, per_command, commands, per_command);
}

int main(int argc, char **argv)
{
    struct suite s;
    char *text;
    long only = -1;
    unsigned long cases = 0, per_command = 0;
    int failures, a, have_cases = 0, have_per = 0;

    for (a = 2; a + 1 < argc; a += 2) {
        if (strcmp(argv[a], "--case") == 0)
            only = (long)number(argv[a + 1], 10, 1000000);
        else if (strcmp(argv[a], "--cases") == 0) {
            cases = number(argv[a + 1], 10, 1000000);
            have_cases = 1;
        } else if (strcmp(argv[a], "--per-command") == 0) {
            per_command = number(argv[a + 1], 10, 1000000);
            have_per = 1;
        } else
            break;
    }
    if (argc < 2 || a != argc || !have_cases || !have_per) {
        printf("usage: %s FILE --cases N --per-command M [--case K]\n", argv[0]);
        return 2;
    }
    file_name = argv[1];
    text = read_file(file_name);
    memset(&s, 0, sizeof s);
    read_suite(&s, text);
    check_counts(&s, cases, per_command);
    if (only >= 0 && (only < 1 || (unsigned long)only > cases)) {
        printf("%s: --case %ld: there is no such case (1..%lu)\n", file_name, only, cases);
        free(s.inputs);
        free(s.cases);
        free(text);
        return 2;
    }

    failures = replay(&s, 0, only);
    failures += replay(&s, 1, only);
    printf("%s: %lu cases, 2 passes, %d failures\n", file_name, only >= 0 ? 1UL : s.n_cases,
           failures);
    free(s.inputs);
    free(s.cases);
    free(text);
    return failures != 0;
}

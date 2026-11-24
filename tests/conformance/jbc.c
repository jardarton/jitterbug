/*
 * Reading and playing conformance lists (jbc.h, FORMAT.md).
 */
#include "jbc.h"

#include <jitterbug.h>

#include <stb_image.h>

#include <stdlib.h>
#include <string.h>

static void say(char *err, size_t size, const char *what, size_t at)
{
    if (err != NULL && size > 0)
        snprintf(err, size, "%s (word %lu)", what, (unsigned long)at);
}

/* A string payload at word i of n words: 1 if its length word and padded
   bytes take exactly words words. */
static int string_ok(const uint32_t *w, size_t i, size_t words)
{
    return words >= 1 && 1 + ((size_t)w[i] + 3) / 4 == words;
}

void jbc_string(const jbc_list *l, size_t i, char *buf, size_t size)
{
    size_t len = l->w[i], k;

    if (size == 0)
        return;
    /* The bytes are stored in the words little-endian, first byte lowest. */
    for (k = 0; k < len && k + 1 < size; ++k)
        buf[k] = (char)((l->w[i + 1 + k / 4] >> (8 * (k % 4))) & 0xff);
    buf[k] = 0;
}

/* Where the GP0 word stream is in a GPU packet (psx-spx, "GPU Command
   Summary", "GPU Render Polygon/Line/Rectangle Commands", "GPU Memory
   Transfer Commands"), so that LOOP and END can be required to fall
   between packets: a player on a console reads VRAM back there with
   GP0(C0h), which inside a packet would become the packet's parameters
   (tools/probe/PROTOCOL.md, "Limits and timeouts"). left: words still due (0:
   between packets); poly: a polyline, ended by a vertex word with
   (w & F000F000h) == 50005000h from its third vertex on. */
typedef struct packet {
    size_t left, at, got;
    int poly, shaded, load;
} packet;

static void packet_word(packet *k, uint32_t w, size_t at)
{
    unsigned c = w >> 24;

    if (k->left == 0 && !k->poly) {
        k->at = at;
        k->got = 1;
        k->load = 0;
        if (c >= 0x20 && c <= 0x3f) { /* polygon: colour, then per vertex xy, uv, colour */
            size_t n = (c & 8) ? 4 : 3;
            k->left = n * (1 + ((c & 4) ? 1 : 0)) + ((c & 0x10) ? n - 1 : 0);
        } else if (c >= 0x40 && c <= 0x5f) { /* line */
            k->shaded = (c & 0x10) != 0;
            if (c & 8)
                k->poly = 1;
            else
                k->left = k->shaded ? 3 : 2;
        } else if (c >= 0x60 && c <= 0x7f) /* rectangle: xy, uv, size if variable */
            k->left = 1 + ((c & 4) ? 1 : 0) + (((c >> 3) & 3) == 0 ? 1 : 0);
        else if (c >= 0x80 && c <= 0x9f) /* VRAM to VRAM */
            k->left = 3;
        else if (c >= 0xa0 && c <= 0xbf) { /* CPU to VRAM: position, size, data */
            k->left = 2;
            k->load = 1;
        } else if ((c >= 0xc0 && c <= 0xdf) || c == 0x02) /* VRAM to CPU, fill */
            k->left = 2;
        return;
    }
    ++k->got;
    if (k->poly) {
        /* Vertex words: flat cmd v0 v1 v2...; shaded cmd v0 c1 v1 c2 v2... */
        size_t idx = k->got - 2, vertex;
        int first = k->shaded ? idx % 2 == 1 : 1;
        vertex = k->shaded ? (idx + 1) / 2 : idx;
        if (first && vertex >= 2 && (w & 0xf000f000u) == 0x50005000u)
            k->poly = 0;
        return;
    }
    --k->left;
    if (k->load && k->got == 3) {
        uint32_t wd = ((w & 0xffffu) - 1) % 1024 + 1, ht = ((w >> 16) - 1) % 512 + 1;
        k->left = ((size_t)wd * ht + 1) / 2;
        k->load = 0;
    }
}

static int validate(jbc_list *l, char *err, size_t size)
{
    size_t i, s, k;
    int ended = 0;
    packet pk;

    if (l->n < 6 || memcmp(l->w, JBC_MAGIC, 8) != 0) {
        say(err, size, "not a conformance list (magic)", 0);
        return -1;
    }
    if (l->w[2] != JBC_VERSION) {
        say(err, size, "unknown version", 2);
        return -1;
    }
    /* The two strings after the fixed header. */
    s = 4;
    for (i = 0; i < 2; ++i) {
        size_t words;
        if (s >= l->n) {
            say(err, size, "header cut short", s);
            return -1;
        }
        words = 1 + ((size_t)l->w[s] + 3) / 4;
        if (s + words > l->n) {
            say(err, size, "header string runs past the end", s);
            return -1;
        }
        jbc_string(l, s, i == 0 ? l->name : l->source, i == 0 ? sizeof l->name : sizeof l->source);
        s += words;
    }
    if (l->w[3] % 4 != 0 || l->w[3] / 4 != s) {
        say(err, size, "header bytes do not match the header", 3);
        return -1;
    }
    l->first = s;
    l->loop = 0;
    l->reads = l->expects = 0;
    memset(&pk, 0, sizeof pk);

    for (i = s; i < l->n && !ended;) {
        unsigned code = JBC_OP_CODE(l->w[i]);
        size_t cnt = JBC_OP_COUNT(l->w[i]);
        const uint32_t *p = l->w + i + 1;
        if (i + 1 + cnt > l->n) {
            say(err, size, "op runs past the end", i);
            return -1;
        }
        if ((code == JBC_LOOP || code == JBC_END) && (pk.left != 0 || pk.poly)) {
            if (err != NULL && size > 0)
                snprintf(err, size, "%s inside a GPU packet: GP0(%02Xh) at word %lu is not complete "
                         "(word %lu)", code == JBC_LOOP ? "LOOP" : "END",
                         (unsigned)(l->w[pk.at] >> 24), (unsigned long)pk.at, (unsigned long)i);
            return -1;
        }
        switch (code) {
        case JBC_END:
            if (cnt != 0 || i + 1 != l->n) {
                say(err, size, "END is not the last word", i);
                return -1;
            }
            ended = 1;
            break;
        case JBC_GP0:
        case JBC_GP0_DMA:
        case JBC_GP1:
            if (cnt == 0) {
                say(err, size, "empty word op", i);
                return -1;
            }
            for (k = 0; k < cnt; ++k)
                if (code != JBC_GP1)
                    packet_word(&pk, p[k], i + 1 + k);
                else if ((p[k] >> 24 & 0x3f) <= 1) /* GP1(00h), GP1(01h) and their
                                                     mirrors (the GPU decodes bits
                                                     24-29): the packet is dropped */
                    memset(&pk, 0, sizeof pk);
            break;
        case JBC_READ:
            if (cnt != 2 || p[0] > JBC_PORT_GPUSTAT || p[1] == 0 || l->loop) {
                say(err, size, "bad READ", i);
                return -1;
            }
            l->reads += p[1];
            break;
        case JBC_WAIT:
            if (cnt != 2) {
                say(err, size, "bad WAIT", i);
                return -1;
            }
            break;
        case JBC_DRAWSYNC:
        case JBC_VSYNC:
            if (cnt != 0) {
                say(err, size, "a payload on an op that has none", i);
                return -1;
            }
            break;
        case JBC_LOOP:
            if (cnt != 0 || l->loop) {
                say(err, size, "bad or second LOOP", i);
                return -1;
            }
            l->loop = i;
            break;
        case JBC_EXPECT:
            if (cnt < 4 || p[0] >= l->reads || !string_ok(l->w, i + 4, cnt - 3) || l->loop) {
                say(err, size, "bad EXPECT", i);
                return -1;
            }
            ++l->expects;
            break;
        case JBC_CALL:
            if (!string_ok(l->w, i + 1, cnt)) {
                say(err, size, "bad CALL", i);
                return -1;
            }
            break;
        default:
            say(err, size, "unknown op", i);
            return -1;
        }
        i += 1 + cnt;
    }
    if (!ended) {
        say(err, size, "no END", l->n);
        return -1;
    }
    return 0;
}

int jbc_load(const char *path, jbc_list *l, char *err, size_t size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *bytes = NULL;
    size_t cap = 0, len = 0, got, i;

    memset(l, 0, sizeof *l);
    if (f == NULL) {
        snprintf(err, size, "cannot open %s", path);
        return -1;
    }
    do {
        if (len == cap) {
            unsigned char *more;
            cap = cap ? cap * 2 : 1 << 20;
            more = realloc(bytes, cap);
            if (more == NULL) {
                free(bytes);
                fclose(f);
                snprintf(err, size, "out of memory");
                return -1;
            }
            bytes = more;
        }
        got = fread(bytes + len, 1, cap - len, f);
        len += got;
    } while (got > 0);
    fclose(f);
    if (len % 4 != 0) {
        free(bytes);
        snprintf(err, size, "%s: not a whole number of words", path);
        return -1;
    }
    l->n = len / 4;
    l->w = malloc(len ? len : 4);
    if (l->w == NULL) {
        free(bytes);
        snprintf(err, size, "out of memory");
        return -1;
    }
    /* Little-endian words, whatever the host. */
    for (i = 0; i < l->n; ++i)
        l->w[i] = (uint32_t)bytes[4 * i] | (uint32_t)bytes[4 * i + 1] << 8 |
                  (uint32_t)bytes[4 * i + 2] << 16 | (uint32_t)bytes[4 * i + 3] << 24;
    free(bytes);
    if (validate(l, err, size) != 0) {
        jbc_free(l);
        return -1;
    }
    return 0;
}

void jbc_free(jbc_list *l)
{
    free(l->w);
    l->w = NULL;
    l->n = 0;
}

int jbc_parse(const uint32_t *words, size_t n, jbc_list *l, char *err, size_t size)
{
    memset(l, 0, sizeof *l);
    l->w = malloc((n ? n : 1) * sizeof *l->w);
    if (l->w == NULL) {
        snprintf(err, size, "out of memory");
        return -1;
    }
    memcpy(l->w, words, n * sizeof *words);
    l->n = n;
    if (validate(l, err, size) != 0) {
        jbc_free(l);
        return -1;
    }
    return 0;
}

#define FNV_OFFSET 0xcbf29ce484222325u
#define FNV_PRIME 0x100000001b3u

static uint64_t fnv_word(uint64_t h, uint32_t w)
{
    int k;

    for (k = 0; k < 4; ++k) {
        h ^= (w >> (8 * k)) & 0xffu;
        h *= FNV_PRIME;
    }
    return h;
}

uint64_t jbc_checksum(const jbc_list *l)
{
    uint64_t h = FNV_OFFSET;
    size_t i;

    for (i = 0; i < l->n; ++i)
        h = fnv_word(h, l->w[i]);
    return h;
}

unsigned char *jbc_read_file(const char *path, size_t *size, const char **why)
{
    enum { JBC_FILE_MAX = 16 * 1024 * 1024 };
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    size_t n = 0, cap = 0;

    *size = 0;
    if (f == NULL) {
        *why = "cannot be opened";
        return NULL;
    }
    for (;;) {
        size_t got;
        if (n == cap) {
            unsigned char *more;
            cap = cap ? cap * 2 : 65536;
            if (cap > (size_t)JBC_FILE_MAX + 1)
                cap = (size_t)JBC_FILE_MAX + 1;
            more = realloc(buf, cap);
            if (more == NULL) {
                *why = "out of memory";
                goto fail;
            }
            buf = more;
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
        if (n > (size_t)JBC_FILE_MAX) {
            *why = "larger than 16 MiB";
            goto fail;
        }
    }
    if (ferror(f)) {
        *why = "cannot be read";
        goto fail;
    }
    fclose(f);
    *size = n;
    return buf;
fail:
    fclose(f);
    free(buf);
    return NULL;
}

uint64_t jbc_bytes_checksum(const unsigned char *bytes, size_t size)
{
    uint64_t h = FNV_OFFSET;
    size_t k;

    for (k = 0; k < size; ++k) {
        h ^= bytes[k];
        h *= FNV_PRIME;
    }
    return h;
}

int jbc_file_checksum(const char *path, uint64_t *sum)
{
    const char *why = NULL;
    size_t size = 0;
    unsigned char *bytes = jbc_read_file(path, &size, &why);

    if (bytes == NULL)
        return -1;
    *sum = jbc_bytes_checksum(bytes, size);
    free(bytes);
    return 0;
}

char *jbc_read_text(const char *path, size_t *size, const char **why)
{
    unsigned char *bytes = jbc_read_file(path, size, why);
    unsigned char *text;

    if (bytes == NULL)
        return NULL;
    if (memchr(bytes, 0, *size) != NULL) {
        *why = "a zero byte in a text file";
        free(bytes);
        return NULL;
    }
    text = realloc(bytes, *size + 1);
    if (text == NULL) {
        *why = "out of memory";
        free(bytes);
        return NULL;
    }
    text[*size] = 0;
    return (char *)text;
}

int jbc_text_line(const char *text, size_t size, size_t *pos, char *line, size_t cap)
{
    const char *start = text + *pos, *end = text + size, *nl;
    size_t len;

    if (*pos >= size)
        return 0;
    nl = memchr(start, '\n', (size_t)(end - start));
    len = nl != NULL ? (size_t)(nl - start) + 1 : (size_t)(end - start);
    if (len + 1 > cap)
        return -1;
    memcpy(line, start, len);
    line[len] = 0;
    *pos += len;
    return 1;
}

int jbc_check_fixture(const jbc_list *l, const jbc_fixture *f, char *err, size_t size)
{
    char name[160];
    uint64_t sum = jbc_checksum(l);
    size_t at = 0;
    int bad = 0;

#define JBC_SAY(...)                                                                  \
    do {                                                                              \
        if (at < size)                                                                \
            at += (size_t)snprintf(err + at, size - at, "%s", bad++ ? "; " : "");     \
        if (at < size)                                                                \
            at += (size_t)snprintf(err + at, size - at, __VA_ARGS__);                 \
    } while (0)

    if (size > 0)
        err[0] = 0;
    if (strchr(f->name, '/') != NULL)
        snprintf(name, sizeof name, "%s", f->name);
    else
        snprintf(name, sizeof name, "gpu/%s", f->name);
    if (strcmp(l->name, name) != 0)
        JBC_SAY("the list is %s, not %s", l->name, name);
    if (l->n != f->words)
        JBC_SAY("%lu words, not %lu", (unsigned long)l->n, f->words);
    if (sum != f->checksum)
        JBC_SAY("checksum %016llx, not %016llx", (unsigned long long)sum,
                (unsigned long long)f->checksum);
    if (l->expects != f->expects)
        JBC_SAY("%u EXPECT ops, not %u", l->expects, f->expects);
    if ((l->loop != 0) != (f->passes != 0))
        JBC_SAY("%s main loop, and %u passes stated", l->loop ? "a" : "no", f->passes);
    if (f->reference == JBC_REF_LOG && f->expects == 0)
        JBC_SAY("compared with a log, but no EXPECT ops stated");
#undef JBC_SAY
    return bad ? -1 : 0;
}

/* Whether s is 1 to max characters, each of the set. */
static int all_of(const char *s, const char *set, size_t max)
{
    size_t n = strlen(s);

    return n >= 1 && n <= max && strspn(s, set) == n;
}

/* One line of a fixture table, split into e: 1 for a line, 0 for a comment
   or blank line, -1 for anything else. Strict, as CMake reads it, single
   spaces or more between the fields: 7 for a probe reference; 8 for a dump
   or log (the REFSUM last); 11 for a dump with an exclusion. */
static int table_line(char *line, jbc_table_entry *e)
{
    static const char digits[] = "0123456789", hex[] = "0123456789abcdef";
    char *f[12], *save = line;
    size_t n = 0, len = strlen(line);
    unsigned long long v;

    if (len > 0 && line[len - 1] == '\n')
        line[--len] = 0;
    if (line[0] == '#' || line[0] == 0)
        return 0;
    while (n < 12) {
        while (*save == ' ')
            ++save;
        if (*save == 0)
            break;
        f[n++] = save;
        while (*save != ' ' && *save != 0)
            ++save;
        if (*save == ' ')
            *save++ = 0;
    }
    if (n < 7 || n > 11 || *save != 0 || !all_of(f[0], "abcdefghijklmnopqrstuvwxyz0123456789-", 100) ||
        (strcmp(f[1], "repo") != 0 && strcmp(f[1], "fetched") != 0 && strcmp(f[1], "open") != 0) ||
        (strcmp(f[2], "dump") != 0 && strcmp(f[2], "log") != 0 && strcmp(f[2], "probe") != 0) ||
        !all_of(f[3], digits, 7) || !all_of(f[4], digits, 7) || !all_of(f[5], digits, 9) ||
        !all_of(f[6], hex, 16))
        return -1;
    memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", f[0]);
    snprintf(e->name, sizeof e->name, "%s%s", strcmp(f[1], "open") == 0 ? "fuzz/" : "", f[0]);
    memcpy(e->where, f[1], strlen(f[1]) + 1); /* at most 8 bytes, checked above */
    e->fixture.name = e->name;
    e->fixture.reference = strcmp(f[2], "dump") == 0 ? JBC_REF_DUMP
                           : strcmp(f[2], "log") == 0 ? JBC_REF_LOG
                                                      : JBC_REF_PROBE;
    e->fixture.passes = (unsigned)strtoul(f[3], NULL, 10);
    e->fixture.expects = (unsigned)strtoul(f[4], NULL, 10);
    e->fixture.words = strtoul(f[5], NULL, 10);
    e->fixture.checksum = (uint64_t)strtoull(f[6], NULL, 16);
    if (e->fixture.reference == JBC_REF_PROBE)
        return n == 7 ? 1 : -1;
    /* A dump or a log: its REFSUM, 16 hex digits; an exclusion only for a
       dump. */
    if ((n != 8 && n != 11) || strlen(f[7]) != 16 || !all_of(f[7], hex, 16) ||
        (n == 11 && e->fixture.reference != JBC_REF_DUMP))
        return -1;
    e->has_ref_checksum = 1;
    e->ref_checksum = (uint64_t)strtoull(f[7], NULL, 16);
    if (n == 11) {
        if (!all_of(f[8], "abcdefghijklmnopqrstuvwxyz0123456789-._", 100) || f[8][0] == '.' ||
            !all_of(f[9], digits, 7) || !all_of(f[10], hex, 16))
            return -1;
        v = strtoull(f[9], NULL, 10);
        snprintf(e->exclude, sizeof e->exclude, "%s", f[8]);
        e->exclude_pixels = (unsigned)v;
        e->exclude_checksum = (uint64_t)strtoull(f[10], NULL, 16);
    }
    return 1;
}

/* The table's line for key (by NAME) or, with key NULL, for the list
   named list_name. The whole table is read and checked first: a malformed
   line, or a NAME on two lines, anywhere in it is an error, whichever line
   is asked for (FORMAT.md, "Fixture tables"). */
static int table_scan(const char *table, const char *key, const char *list_name, jbc_table_entry *e,
                      char *err, size_t size)
{
    /* The file is read whole, once; a zero byte in it is refused, so no
       part of a line can hide from the checks behind one. */
    const char *why = NULL;
    size_t tsize = 0, tpos = 0;
    char *t = jbc_read_text(table, &tsize, &why);
    char line[512];
    unsigned lineno = 0, n = 0, cap = 0, k, found = 0;
    int more_lines;
    jbc_table_entry cur;
    struct seen {
        char key[128];
        unsigned line;
    } *seen = NULL;

    if (t == NULL) {
        snprintf(err, size, "%s: %s", table, why);
        return -1;
    }
    while ((more_lines = jbc_text_line(t, tsize, &tpos, line, sizeof line)) != 0) {
        int r;
        ++lineno;
        if (more_lines < 0) {
            snprintf(err, size, "%s, line %u: too long", table, lineno);
            goto fail;
        }
        r = table_line(line, &cur);
        if (r < 0) {
            snprintf(err, size, "%s, line %u: malformed", table, lineno);
            goto fail;
        }
        if (r == 0)
            continue;
        for (k = 0; k < n; ++k)
            if (strcmp(seen[k].key, cur.key) == 0) {
                snprintf(err, size, "%s, line %u: a second line for %s (the first is line %u)",
                         table, lineno, cur.key, seen[k].line);
                goto fail;
            }
        if (n == cap) {
            struct seen *more;
            cap = cap ? cap * 2 : 64;
            more = realloc(seen, cap * sizeof *seen);
            if (more == NULL) {
                snprintf(err, size, "out of memory");
                goto fail;
            }
            seen = more;
        }
        memcpy(seen[n].key, cur.key, sizeof cur.key);
        seen[n++].line = lineno;
        if (key != NULL) {
            if (strcmp(cur.key, key) != 0)
                continue;
        } else {
            char full[160];
            snprintf(full, sizeof full, "%s/%s", strcmp(cur.where, "open") == 0 ? "fuzz" : "gpu",
                     cur.key);
            if (strcmp(full, list_name) != 0)
                continue;
        }
        /* Names are unique (checked above), so this is the only match. */
        found = 1;
        *e = cur;
        e->fixture.name = e->name;
    }
    free(t);
    free(seen);
    if (!found) {
        snprintf(err, size, "%s has no line for %s", table, key != NULL ? key : list_name);
        return -1;
    }
    return 0;
fail:
    free(t);
    free(seen);
    return -1;
}

int jbc_table_lookup(const char *table, const char *key, jbc_table_entry *e, char *err, size_t size)
{
    return table_scan(table, key, NULL, e, err, size);
}

int jbc_fixture_find(const char *table, const jbc_list *l, jbc_fixture *f, char *name,
                     size_t name_size, char *where, char *err, size_t size)
{
    jbc_table_entry e;

    if (table_scan(table, NULL, l->name, &e, err, size) != 0)
        return -1;
    snprintf(name, name_size, "%s", e.name);
    memcpy(where, e.where, strlen(e.where) + 1);
    *f = e.fixture;
    f->name = name;
    return 0;
}

int jbc_append(const jbc_list *l, const uint32_t *words, size_t n, jbc_list *out)
{
    char err[64];

    *out = *l;
    out->w = malloc((l->n + n) * sizeof *out->w);
    if (out->w == NULL)
        return -1;
    memcpy(out->w, l->w, (l->n - 1) * sizeof *l->w);
    memcpy(out->w + l->n - 1, words, n * sizeof *words);
    out->w[l->n - 1 + n] = l->w[l->n - 1];
    out->n = l->n + n;
    if (validate(out, err, sizeof err) != 0) {
        jbc_free(out);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */

typedef struct {
    jb_context *jb;
    const jbc_options *o;
    uint32_t *buf; /* GP0 words not yet sent (JBC_SPLIT_RANDOM) */
    size_t nbuf, cap;
    uint32_t rng;
    jbc_outcome *out;
    size_t cap_reads;
    int oom;
} Player;

static uint32_t next_random(Player *p)
{
    /* xorshift32 */
    p->rng ^= p->rng << 13;
    p->rng ^= p->rng >> 17;
    p->rng ^= p->rng << 5;
    return p->rng;
}

static void send_gp0(Player *p, const uint32_t *w, size_t n)
{
    size_t i;

    switch (p->o->split) {
    case JBC_SPLIT_OP:
        jb_gp0(p->jb, w, n);
        break;
    case JBC_SPLIT_WORDS:
        for (i = 0; i < n; ++i)
            jb_gp0(p->jb, w + i, 1);
        break;
    case JBC_SPLIT_RANDOM:
        if (p->nbuf + n > p->cap) {
            size_t cap = (p->nbuf + n) * 2;
            uint32_t *more = realloc(p->buf, cap * sizeof *more);
            if (more == NULL) {
                p->oom = 1;
                return;
            }
            p->buf = more;
            p->cap = cap;
        }
        memcpy(p->buf + p->nbuf, w, n * sizeof *w);
        p->nbuf += n;
        break;
    }
}

/* Sends the words held back, in chunks of 1 to 64 words, or now and then
   up to 4096. */
static void flush(Player *p)
{
    size_t at = 0;

    while (at < p->nbuf) {
        uint32_t r = next_random(p);
        size_t chunk = 1 + (r >> 8) % ((r & 7) == 0 ? 4096 : 64);
        if (chunk > p->nbuf - at)
            chunk = p->nbuf - at;
        jb_gp0(p->jb, p->buf + at, chunk);
        at += chunk;
    }
    p->nbuf = 0;
}

static void add_read(Player *p, uint32_t v)
{
    jbc_outcome *out = p->out;

    if (out->nreads == p->cap_reads) {
        size_t cap = p->cap_reads ? p->cap_reads * 2 : 64;
        uint32_t *more = realloc(out->reads, cap * sizeof *more);
        if (more == NULL) {
            p->oom = 1;
            return;
        }
        out->reads = more;
        p->cap_reads = cap;
    }
    out->reads[out->nreads++] = v;
}

static void wait_for(Player *p, uint32_t mask, uint32_t value, const char *what, size_t at)
{
    uint32_t stat = jb_gpustat(p->jb);

    ++p->out->waits;
    if ((stat & mask) == value)
        return;
    ++p->out->waits_failed;
    if (p->o->log != NULL && p->out->waits_failed <= 10)
        fprintf(p->o->log,
                "wait at word %lu (%s) never ends: GPUSTAT 0x%08x, & 0x%08x is not 0x%08x\n",
                (unsigned long)at, what, stat, mask, value);
}

/*
 * The GPU state the public API can read back besides VRAM (FORMAT.md,
 * "Playing a list"): GPUSTAT, and GP1(10h)'s answers for indices 2 to 5
 * (the texture window, the drawing area's top left and bottom right
 * corners, the drawing offset, as jb_gp1 documents them). It is read on
 * the replay itself. GP1(10h) changes only GPUREAD's latched value, which
 * nothing played after LOOP can see (READ is refused there), and it is
 * read only when no VRAM-to-CPU data is waiting (GPUSTAT bit 27 clear):
 * with data waiting, jb_gpuread would hand out that data and so change the
 * replay, and the state is reported unreadable instead.
 */
enum { STATE_INFO = 4 };

typedef struct gpu_state {
    uint32_t stat;
    uint32_t info[STATE_INFO];
    int pending;
} gpu_state;

static void read_state(jb_context *jb, gpu_state *st)
{
    unsigned k;

    memset(st, 0, sizeof *st);
    st->stat = jb_gpustat(jb);
    st->pending = (st->stat >> 27 & 1u) != 0;
    if (st->pending)
        return;
    for (k = 0; k < STATE_INFO; ++k) {
        jb_gp1(jb, 0x10000000u | (2u + k));
        st->info[k] = jb_gpuread(jb);
    }
}

/* The first difference between the state kept after pass `at` and the
   state after pass `pass`, into the outcome; 1 if there is one. */
static int state_differs(jbc_outcome *out, const gpu_state *kept, unsigned at, const gpu_state *now,
                         unsigned pass)
{
    static const char *const what[STATE_INFO] = {
        "the texture window (GP1(10h) index 2)", "the drawing area's top left (GP1(10h) index 3)",
        "the drawing area's bottom right (GP1(10h) index 4)", "the drawing offset (GP1(10h) index 5)"};
    unsigned k;

    if (kept->pending || now->pending) {
        snprintf(out->state_diff, sizeof out->state_diff,
                 "VRAM-to-CPU data is waiting after pass %u (GPUSTAT bit 27), so the state cannot be "
                 "read back without changing the replay",
                 kept->pending ? at : pass);
        return 1;
    }
    if (kept->stat != now->stat) {
        snprintf(out->state_diff, sizeof out->state_diff, "GPUSTAT: 0x%08x after pass %u, 0x%08x after pass %u",
                 kept->stat, at, now->stat, pass);
        return 1;
    }
    for (k = 0; k < STATE_INFO; ++k)
        if (kept->info[k] != now->info[k]) {
            snprintf(out->state_diff, sizeof out->state_diff, "%s: 0x%08x after pass %u, 0x%08x after pass %u",
                     what[k], kept->info[k], at, now->info[k], pass);
            return 1;
        }
    return 0;
}

int jbc_play(const jbc_list *l, const jbc_options *o, jbc_outcome *out, char *err, size_t size)
{
    jb_config cfg;
    jb_result r = JB_OK;
    Player p;
    gpu_state kept_state, state;
    uint16_t *prev = NULL, *kept = NULL;
    size_t i, body = 0;
    unsigned max_loops = o->max_loops ? o->max_loops : 1000;
    unsigned window = o->window ? o->window : 2;

    memset(out, 0, sizeof *out);
    memset(&p, 0, sizeof p);
    memset(&cfg, 0, sizeof cfg);
    cfg.scale = o->scale ? o->scale : 1;
    /* The display size jb_frame falls back to after GP1(00h): all of VRAM,
       for jbc_options.picture. The programs set their own modes. */
    cfg.width = JBC_VRAM_W;
    cfg.height = JBC_VRAM_H;
    cfg.threads = o->threads ? o->threads : 1;
    p.jb = jb_create(&cfg, &r);
    if (p.jb == NULL) {
        snprintf(err, size, "jb_create: %s", jb_result_string(r));
        return -1;
    }
    p.o = o;
    p.out = out;
    p.rng = o->seed ? o->seed : 1;
    out->vram = malloc(JBC_VRAM_PIXELS * sizeof *out->vram);
    prev = malloc(JBC_VRAM_PIXELS * sizeof *prev);
    kept = malloc(JBC_VRAM_PIXELS * sizeof *kept);
    if (out->vram == NULL || prev == NULL || kept == NULL) {
        free(prev);
        free(kept);
        jb_destroy(p.jb);
        jbc_outcome_free(out);
        snprintf(err, size, "out of memory");
        return -1;
    }
    out->steady = 1;
    out->state_steady = 1;
    out->window = o->passes ? window : 0;
    memset(&kept_state, 0, sizeof kept_state);

    for (i = l->first;;) {
        uint32_t op = l->w[i];
        unsigned code = JBC_OP_CODE(op);
        size_t cnt = JBC_OP_COUNT(op), k;
        const uint32_t *w = l->w + i + 1;

        if (code == JBC_GP0 || code == JBC_GP0_DMA) {
            send_gp0(&p, w, cnt);
            i += 1 + cnt;
            continue;
        }
        if (code == JBC_CALL) {
            i += 1 + cnt;
            continue;
        }
        flush(&p);
        switch (code) {
        case JBC_GP1:
            for (k = 0; k < cnt; ++k)
                jb_gp1(p.jb, w[k]);
            break;
        case JBC_READ:
            for (k = 0; k < w[1]; ++k)
                add_read(&p, w[0] == JBC_PORT_GPUSTAT ? jb_gpustat(p.jb) : jb_gpuread(p.jb));
            break;
        case JBC_WAIT:
            wait_for(&p, w[0], w[1], "WAIT", i);
            break;
        case JBC_DRAWSYNC: {
            /* The SDK's DrawSync(0): ready for a DMA block (bit 28), and
               with a DMA direction set (bits 29-30) also ready for
               commands (bit 26). */
            uint32_t need = (jb_gpustat(p.jb) >> 29 & 3) ? 0x14000000u : 0x10000000u;
            wait_for(&p, need, need, "DRAWSYNC", i);
            break;
        }
        case JBC_VSYNC:
            break;
        case JBC_EXPECT: {
            char label[512];
            uint32_t got = w[0] < out->nreads ? out->reads[w[0]] : 0;
            int ok = (got & w[1]) == w[2];
            ++out->checks;
            out->checks_failed += !ok;
            jbc_string(l, i + 4, label, sizeof label);
            if (o->log != NULL) {
                if (ok)
                    fprintf(o->log, "pass - %s\n", label);
                else
                    fprintf(o->log,
                            "fail - %s: read %u was 0x%08x, & 0x%08x = 0x%x; the console's: 0x%x\n",
                            label, (unsigned)w[0], got, w[1], got & w[1], w[2]);
            }
            break;
        }
        case JBC_LOOP:
            /* VRAM after pass 1. */
            jb_vram_read(p.jb, 0, 0, JBC_VRAM_W, JBC_VRAM_H, prev);
            if (o->passes == 1) {
                memcpy(kept, prev, JBC_VRAM_PIXELS * sizeof *kept);
                read_state(p.jb, &kept_state);
            }
            out->passes = o->passes;
            body = i + 1;
            break;
        case JBC_END: {
            /* The end of a pass of the main loop. Pass 1 is everything
               before LOOP (which includes the loop's first time round);
               it ends at the LOOP op. Each time the body ends here is the
               next pass: 2, 3, ... */
            unsigned pass;
            int same;

            jb_vram_read(p.jb, 0, 0, JBC_VRAM_W, JBC_VRAM_H, out->vram);
            if (body == 0)
                goto done;
            pass = ++out->loops + 1;
            same = memcmp(prev, out->vram, JBC_VRAM_PIXELS * sizeof *prev) == 0;
            if (o->passes == 0) {
                /* Until a pass leaves VRAM as it found it: the state after
                   the pass before. */
                out->passes = pass - 1;
                if (same)
                    goto done;
                if (out->loops >= max_loops) {
                    out->steady = 0;
                    out->changed_at = pass;
                    goto done;
                }
            } else if (pass == o->passes) {
                /* The stated pass, the state compared. It must be the first
                   of the fixed point: the pass before changed VRAM. */
                out->early = same;
                memcpy(kept, out->vram, JBC_VRAM_PIXELS * sizeof *kept);
                read_state(p.jb, &kept_state);
            } else if (pass > o->passes) {
                /* The window: nothing may change, in VRAM or in the state
                   read back. A change of state alone does not end the
                   window early, so that VRAM is judged as the probe
                   judges it. */
                if (memcmp(kept, out->vram, JBC_VRAM_PIXELS * sizeof *kept) != 0) {
                    out->steady = 0;
                    out->changed_at = pass;
                }
                read_state(p.jb, &state);
                ++out->window_played;
                if (out->state_steady && state_differs(out, &kept_state, o->passes, &state, pass)) {
                    out->state_steady = 0;
                    out->state_changed_at = pass;
                }
                if (!out->steady || pass == o->passes + window) {
                    memcpy(out->vram, kept, JBC_VRAM_PIXELS * sizeof *kept);
                    goto done;
                }
            }
            memcpy(prev, out->vram, JBC_VRAM_PIXELS * sizeof *prev);
            i = body;
            continue;
        }
        }
        if (p.oom)
            break;
        i += 1 + cnt;
    }
done:
    if (o->picture && !p.oom) {
        /* GP1(00h) keeps VRAM and gives the configured display size back;
           then the display is enabled at (0, 0). */
        const unsigned char *f;
        int w = 0, h = 0;

        jb_gp1(p.jb, 0x00000000u);
        jb_gp1(p.jb, 0x05000000u);
        jb_gp1(p.jb, 0x03000000u);
        f = jb_frame(p.jb, &w, &h);
        if (f != NULL && (out->picture = malloc((size_t)w * h * 4)) != NULL) {
            memcpy(out->picture, f, (size_t)w * h * 4);
            out->picture_w = w;
            out->picture_h = h;
        }
    }
    free(prev);
    free(kept);
    free(p.buf);
    jb_destroy(p.jb);
    if (p.oom) {
        jbc_outcome_free(out);
        snprintf(err, size, "out of memory");
        return -1;
    }
    return 0;
}

void jbc_describe_loop(const jbc_outcome *r, char *buf, size_t size)
{
    char played[64] = "";
    size_t at;
    unsigned last = r->passes + r->window_played;

    if (size == 0)
        return;
    if (r->window_played == 1)
        snprintf(played, sizeof played, "pass %u", last);
    else if (r->window_played > 1)
        snprintf(played, sizeof played, "passes %u-%u", r->passes + 1, last);
    /* VRAM. */
    if (!r->steady)
        snprintf(buf, size, "main loop: compared at pass %u, but pass %u changed VRAM", r->passes,
                 r->changed_at);
    else if (r->early)
        snprintf(buf, size,
                 "main loop: compared at pass %u, but VRAM had settled at pass %u already (the "
                 "fixture's pass count is wrong)",
                 r->passes, r->passes - 1);
    else if (r->window_played == 0)
        snprintf(buf, size, "main loop: compared after pass %u; no pass of the window was played",
                 r->passes);
    else
        snprintf(buf, size, "main loop: compared after pass %u%s; %s did not change VRAM", r->passes,
                 r->passes > 1 ? ", which changed VRAM" : "", played);
    at = strlen(buf);
    if (r->window_played < r->window && at < size)
        at += (size_t)snprintf(buf + at, size - at, " (%s%u of the %u passes of the window played)",
                               r->steady ? "only " : "the window ends there: ", r->window_played,
                               r->window);
    if (at + 1 >= size)
        return;
    buf[at++] = '\n';
    buf[at] = 0;
    /* The state read back, of the passes played only. */
    if (!r->state_steady)
        snprintf(buf + at, size - at, "main loop: compared at pass %u, but pass %u changed the GPU state: %s",
                 r->passes, r->state_changed_at, r->state_diff);
    else if (r->window_played == 0)
        snprintf(buf + at, size - at, "main loop: the GPU state was not checked after pass %u",
                 r->passes);
    else
        snprintf(buf + at, size - at,
                 "main loop: %s left GPUSTAT and GP1(10h) 2-5 as pass %u did%s (the CLUT cache and "
                 "other state the API cannot read are not compared: FORMAT.md)",
                 played, r->passes,
                 r->window_played < r->window ? "; the rest of the window was not played" : "");
}

void jbc_outcome_free(jbc_outcome *out)
{
    free(out->vram);
    free(out->reads);
    free(out->picture);
    out->vram = NULL;
    out->reads = NULL;
    out->picture = NULL;
}

/* ------------------------------------------------------------------------ */

uint16_t *jbc_load_dump(const char *path, const char **why)
{
    size_t size = 0;
    unsigned char *bytes = jbc_read_file(path, &size, why);
    uint16_t *pix;

    if (bytes == NULL)
        return NULL;
    pix = jbc_decode_dump(bytes, size, why);
    free(bytes);
    return pix;
}

uint16_t *jbc_decode_dump(const unsigned char *bytes, size_t size, const char **why)
{
    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = size <= 0x7fffffffu ? stbi_load_from_memory(bytes, (int)size, &w, &h, &comp, 4) : NULL;
    uint16_t *pix;
    size_t i;

    if (rgba == NULL) {
        *why = stbi_failure_reason();
        return NULL;
    }
    if (w != JBC_VRAM_W || h != JBC_VRAM_H) {
        stbi_image_free(rgba);
        *why = "not 1024 x 512";
        return NULL;
    }
    pix = malloc(JBC_VRAM_PIXELS * sizeof *pix);
    if (pix == NULL) {
        stbi_image_free(rgba);
        *why = "out of memory";
        return NULL;
    }
    for (i = 0; i < JBC_VRAM_PIXELS; ++i) {
        const unsigned char *o = rgba + 4 * i;
        if (((o[0] | o[1] | o[2]) & 7) != 0 || o[3] != 255) {
            free(pix);
            stbi_image_free(rgba);
            *why = "a pixel that is not a 15-bit colour shifted left by 3, opaque";
            return NULL;
        }
        pix[i] = (uint16_t)(o[0] >> 3 | (o[1] >> 3) << 5 | (o[2] >> 3) << 10);
    }
    stbi_image_free(rgba);
    return pix;
}

void jbc_compare(const uint16_t *ours, const uint16_t *dump, jbc_diff *d)
{
    jbc_compare_excluding(ours, dump, NULL, d);
}

/* One line of an exclusion file (without its newline): 0, or -1 with the
   reason in err. */
static int exclusion_line(const char *path, unsigned number, const char *line,
                          unsigned char *excluded, unsigned *pixels, unsigned *entries, char *err,
                          size_t size)
{
    int x, y, dx, dy, n, w, h, used = 0, i, j, k;
    char word[16];

    if (line[0] == '#' || line[0] == 0 || sscanf(line, "%15s", word) != 1)
        return 0;
    if (strcmp(word, "run") == 0 &&
        sscanf(line, "%*s %d %d %d %d %d %n", &x, &y, &dx, &dy, &n, &used) == 5 && used > 0 &&
        n > 0 && dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) {
        w = n;
        h = 1;
    } else if (strcmp(word, "rect") == 0 &&
               sscanf(line, "%*s %d %d %d %d %n", &x, &y, &w, &h, &used) == 4 && used > 0 &&
               w > 0 && h > 0) {
        dx = dy = 0;
        n = 0;
    } else {
        snprintf(err, size, "%s:%u: not \"run X Y DX DY N reason\" or \"rect X Y W H reason\"",
                 path, number);
        return -1;
    }
    while (line[used] == ' ' || line[used] == '\t')
        ++used;
    if (line[used] == 0) {
        snprintf(err, size, "%s:%u: no reason given", path, number);
        return -1;
    }
    for (j = 0; j < h; ++j)
        for (i = 0; i < w; ++i) {
            int px = n ? x + i * dx : x + i, py = n ? y + i * dy : y + j;
            if (px < 0 || px >= JBC_VRAM_W || py < 0 || py >= JBC_VRAM_H) {
                snprintf(err, size, "%s:%u: (%d, %d) is outside VRAM", path, number, px, py);
                return -1;
            }
            k = py * JBC_VRAM_W + px;
            *pixels += !excluded[k];
            excluded[k] = 1;
        }
    ++*entries;
    return 0;
}

int jbc_parse_exclusions(const char *text, const char *name, unsigned char *excluded,
                         unsigned *pixels, unsigned *entries, char *err, size_t size)
{
    char line[512];
    unsigned number = 0;

    *pixels = *entries = 0;
    while (*text) {
        size_t len = strcspn(text, "\n");
        if (len >= sizeof line) {
            snprintf(err, size, "%s:%u: line too long", name, number + 1);
            return -1;
        }
        memcpy(line, text, len);
        line[len] = 0;
        if (len > 0 && line[len - 1] == '\r')
            line[len - 1] = 0;
        text += len + (text[len] == '\n');
        if (exclusion_line(name, ++number, line, excluded, pixels, entries, err, size) != 0)
            return -1;
    }
    return 0;
}

int jbc_load_exclusions(const char *path, unsigned char *excluded, unsigned *pixels,
                        unsigned *entries, char *err, size_t size)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long n;
    int r;

    *pixels = *entries = 0;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        if (f != NULL)
            fclose(f);
        snprintf(err, size, "cannot read %s", path);
        return -1;
    }
    text = malloc((size_t)n + 1);
    if (text == NULL || fread(text, 1, (size_t)n, f) != (size_t)n) {
        free(text);
        fclose(f);
        snprintf(err, size, "cannot read %s", path);
        return -1;
    }
    fclose(f);
    text[n] = 0;
    r = jbc_parse_exclusions(text, path, excluded, pixels, entries, err, size);
    free(text);
    return r;
}

void jbc_compare_excluding(const uint16_t *ours, const uint16_t *dump,
                           const unsigned char *excluded, jbc_diff *d)
{
    jbc_compare_bits(ours, dump, excluded, 0x7fffu, d);
}

void jbc_compare_bits(const uint16_t *ours, const uint16_t *dump, const unsigned char *excluded,
                      unsigned bits, jbc_diff *d)
{
    int x, y;

    d->count = 0;
    d->x0 = JBC_VRAM_W;
    d->y0 = JBC_VRAM_H;
    d->x1 = d->y1 = -1;
    for (y = 0; y < JBC_VRAM_H; ++y)
        for (x = 0; x < JBC_VRAM_W; ++x) {
            size_t i = (size_t)y * JBC_VRAM_W + x;
            if ((ours[i] & bits) == (dump[i] & bits) || (excluded != NULL && excluded[i]))
                continue;
            ++d->count;
            d->x0 = x < d->x0 ? x : d->x0;
            d->y0 = y < d->y0 ? y : d->y0;
            d->x1 = x > d->x1 ? x : d->x1;
            d->y1 = y > d->y1 ? y : d->y1;
        }
}

uint64_t jbc_exclusion_checksum(const unsigned char *excluded)
{
    uint64_t h = FNV_OFFSET;
    uint32_t i;

    for (i = 0; i < JBC_VRAM_PIXELS; ++i)
        if (excluded[i])
            h = fnv_word(h, i);
    return h;
}

int jbc_judge(const char *name, const jbc_verdict *v, char *line, size_t size)
{
    int compared = v->vram_differs || v->checks_failed != 0 || v->reads_differ != 0;
    int unplayable = v->waits_failed != 0 || v->loop_failed;
    char excluded[64] = "";

    if (v->excluded)
        snprintf(excluded, sizeof excluded, " (%u pixels excluded)", v->excluded);
    if (unplayable) {
        snprintf(line, size, "%s: DIFFERS: %s%s%s", name,
                 v->waits_failed ? "a wait that would never end" : "",
                 v->waits_failed && v->loop_failed ? ", " : "",
                 v->loop_failed ? "the main loop is not at its fixed point at the stated pass" : "");
        if (v->expect_mismatch && size > 0) {
            size_t at = strlen(line);
            snprintf(line + at, size - at,
                     " (listed in tests/conformance/expected-failures.txt, but only a VRAM or "
                     "check difference is expected there)");
        }
        return 1;
    }
    if (v->console_checks_failed) {
        /* The reference's console disagrees with the list's own values:
           not jitterbug's to settle, and never a pass. */
        snprintf(line, size,
                 "%s: %s the console's result, but the console failed %u of the list's checks: it "
                 "differs from the console the list's values came from (see above; "
                 "docs/conformance.md)",
                 name, compared ? "DIFFERS from" : "MATCHES", v->console_checks_failed);
        return 1;
    }
    if (v->expect_mismatch) {
        if (compared) {
            snprintf(line, size, "%s: DIFFERS%s, as expected (tests/conformance/expected-failures.txt)",
                     name, excluded);
            return 0;
        }
        snprintf(line, size,
                 "%s: MATCHES%s, but is listed in tests/conformance/expected-failures.txt: remove "
                 "it from the list",
                 name, excluded);
        return 1;
    }
    snprintf(line, size, "%s: %s%s", name, compared ? "DIFFERS" : "MATCHES", excluded);
    return compared;
}

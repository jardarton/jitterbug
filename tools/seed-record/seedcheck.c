/*
 * seedcheck: parses seed traces (tests/seed/FORMAT.md) and prints what each
 * holds. Written from FORMAT.md alone, to show it is enough. Exits 1 if any
 * file is malformed. Usage: seedcheck FILE.jbt...
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char *buf;
static size_t len, pos;
static const char *file;

static void fail(const char *what)
{
    fprintf(stderr, "%s: offset %zu: %s\n", file, pos, what);
    exit(1);
}

static uint32_t u32(void)
{
    uint32_t v;
    if (len - pos < 4)
        fail("truncated");
    v = buf[pos] | (uint32_t)buf[pos + 1] << 8 | (uint32_t)buf[pos + 2] << 16 |
        (uint32_t)buf[pos + 3] << 24;
    pos += 4;
    return v;
}

/* Bytes of u16[n], padded. */
static size_t u16s(uint32_t n)
{
    return ((size_t)n * 2 + 3) & ~(size_t)3;
}

static const char *const names[0x33] = {
    [0x00] = "END", [0x01] = "GP0", [0x02] = "FILL", [0x03] = "LOAD", [0x04] = "STORE",
    [0x05] = "COPY", [0x06] = "FLUSH", [0x07] = "SYNC", [0x08] = "RESET_STATE",
    [0x09] = "HIRES_REPLICATE", [0x0a] = "LOWRES_MARK", [0x0b] = "LOWRES_RESOLVE",
    [0x0c] = "TRACE_CALL", [0x0d] = "TRACE_TIME", [0x10] = "CONFIGURE",
    [0x11] = "SET_EXACT_1X", [0x12] = "SET_TEXTURE_MAPPING", [0x13] = "VRAM_GET",
    [0x20] = "PRECISE_ENABLE", [0x21] = "PRECISE_BEGIN_FRAME", [0x22] = "PRECISE_RECORD",
    [0x23] = "PRECISE_STATS", [0x30] = "PRESENT", [0x31] = "CAPTURE", [0x32] = "POKE",
};
/* Fixed payload sizes; -1 for the variable ones. */
static const int fixed[0x33] = {
    [0x00] = 0, [0x01] = -1, [0x02] = 20, [0x03] = -1, [0x04] = -1, [0x05] = 24, [0x06] = 0,
    [0x07] = 0, [0x08] = 0, [0x09] = 0, [0x0a] = 16, [0x0b] = 16, [0x0c] = 0, [0x0d] = 4,
    [0x10] = 20, [0x11] = 4, [0x12] = 4, [0x13] = 0, [0x20] = 4, [0x21] = 0, [0x22] = 16,
    [0x23] = 8, [0x30] = 56, [0x31] = 36, [0x32] = -1,
};

static void check(const char *path)
{
    unsigned count[0x33] = {0}, ops = 0, standalone = 0, records = 0, words = 0;
    unsigned scales = 0, threads_seen[65] = {0}, persp = 0, scale, i;
    uint32_t hdr, nlen, n;
    char name[256];
    const char *sep;
    FILE *f = fopen(path, "rb");
    long size = 0;
    unsigned char *data;

    file = path;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0)
        fail("cannot read");
    rewind(f);
    data = malloc((size_t)size + 1);
    if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size)
        fail("cannot read");
    fclose(f);
    buf = data, len = (size_t)size, pos = 0;

    if (len < 8 || memcmp(buf, "JBSEEDT\0", 8) != 0)
        fail("bad magic");
    pos = 8;
    if (u32() != 1)
        fail("unknown version");
    hdr = u32();
    (void)u32(); /* scene index */
    nlen = u32();
    if (nlen >= sizeof name || len - pos < nlen)
        fail("bad name");
    memcpy(name, buf + pos, nlen);
    name[nlen] = 0;
    pos += (nlen + 3) & ~3u;
    scale = u32();
    scales |= 1u << (scale & 15);
    (void)u32(); /* dither */
    n = u32();   /* threads */
    threads_seen[n <= 64 ? n : 0] = 1;
    (void)u32(); /* exact_1x */
    persp |= u32();
    pos += 4 * 5; /* spans, precise enabled, lookups, hits */
    n = u32();
    if (n > 2048 || len - pos < (size_t)n * 16)
        fail("bad precise table");
    pos += (size_t)n * 16;
    pos += 4 + 8 + 8;
    if (pos != hdr)
        fail("header bytes disagree with the prelude");

    for (;;) {
        uint32_t code = u32(), plen = u32();
        size_t start = pos, want = plen;

        if (plen % 4 != 0 || len - pos < plen)
            fail("bad op length");
        if (code > 0x32 || names[code] == NULL)
            fail("unknown op");
        ++ops;
        ++count[code];
        if (fixed[code] >= 0) {
            want = (size_t)fixed[code];
        } else if (code == 0x01) {
            n = u32();
            want = 8 + (size_t)n * 4;
            words += n;
        } else if (code == 0x03 || code == 0x04) {
            pos += 16;
            n = u32();
            want = 20 + u16s(n);
        } else if (code == 0x32) {
            uint64_t total = 0, wh;
            if (u32() > 1)
                fail("bad POKE plane");
            wh = (uint64_t)u32();
            wh *= u32();
            n = u32();
            if (plen != 16 + (size_t)n * 8)
                fail("POKE runs disagree with the length");
            for (i = 0; i < n; ++i) {
                total += u32(); /* count */
                (void)u32();    /* value */
            }
            if (total != wh)
                fail("POKE runs do not cover the plane");
        }
        if (want != plen)
            fail("payload length is wrong for this op");
        pos = start;
        if (code == 0x10) {
            scale = u32();
            scales |= 1u << (scale & 15);
            pos += 4;
            n = u32();
            threads_seen[n <= 64 ? n : 0] = 1;
        } else if (code == 0x12) {
            persp |= u32();
        } else if (code == 0x22) {
            ++records;
        } else if (code == 0x31) {
            uint32_t kind = u32();
            if (kind < 1 || kind > 5)
                fail("bad CAPTURE kind");
            standalone += u32() == 1;
        }
        pos = start + plen;
        if (code == 0x00) {
            if (pos != len)
                fail("data after END");
            break;
        }
    }

    printf("%s: %u ops (", name, ops);
    for (i = 1, sep = ""; i <= 0x32; ++i)
        if (count[i] != 0) {
            printf("%s%s %u", sep, names[i], count[i]);
            sep = ", ";
        }
    printf("); %u GP0 words; captures %u (%u standalone); scales", words, count[0x31],
           standalone);
    for (i = 1; i < 16; ++i)
        if (scales & (1u << i))
            printf(" %ux", i);
    printf("; threads");
    for (i = 1; i <= 64; ++i)
        if (threads_seen[i])
            printf(" %u", i);
    printf("; precise records %u; perspective %s; presents %u\n", records, persp ? "on" : "off",
           count[0x30]);
    free(data);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; ++i)
        check(argv[i]);
    printf("%d traces parsed\n", argc - 1);
    return 0;
}
